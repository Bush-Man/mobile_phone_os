/*
 * compositor.c - the UI compositor daemon (phase 15, plan item 79).
 *
 * Owns /dev/fb0 and /dev/event0. Apps open windows over the
 * phase-8 unix transport (/var/run/ui): each window is one shm
 * object the app draws into and the compositor composites into a
 * private full-screen stage, presenting dirty frames through the
 * fb0 blit ioctl. The compositor also draws all chrome: status bar
 * (battery/signal/time, item 83), notification banners (item 83),
 * the lock screen with PIN (item 81) and the home launcher grid
 * (item 81). Input records are routed to the focused window as
 * UI_EVENT messages; taps on compositor-owned surfaces (lockscreen
 * numpad, launcher icons, home bar) are handled here.
 *
 * Threads (pthread-lite over kernel tasks):
 *   main    -- blocks on /dev/event0, routes taps + forwards
 *              press/release to the focused window
 *   render  -- 100 ms tick; recomposites + blits when anything
 *              changed (also the 1 Hz clock/battery refresher)
 *   server  -- accept() loop; one reader thread per connection
 *   modem   -- /var/run/modem client; EV SMS/CALL lines become
 *              banners + status, SIGNAL replies feed the bars
 *
 * A single mutex guards window state + the stage; the framebuffer
 * ioctl is issued under it too, so a frame is never torn mid-blit.
 * No heap is used anywhere: all tables are static.
 *
 * Modem events ride a second client connection to /var/run/modem
 * (kernel modemd): incoming SMS and call state changes become
 * banners + status icons.
 */

#include "libc.h"
#include "sysinfo.h"
#include "ui.h"

/* ---- fb0 ioctl ABI (mirror of kernel/syscall.c fb0_ioctl) ---------- */

#define FBIO_BLIT   1u
#define FBIO_INFO   3u

struct fbio_blit {
    u32 x, y, w, h;
    u64 src;
};

struct fbio_info {
    u32 w, h, bpp;
};

/* input wire record (mirror of include/input.h)                    */
struct wire_ev {
    u32 ms;
    u16 type;
    u16 code;
    i32 value;
} __attribute__((packed));

#define EV_SYN      0u
#define EV_KEY      1u
#define EV_ABS      3u
#define ABS_X       0u
#define ABS_Y       1u
#define BTN_TOUCH   330u
#define KEY_POWER   116u
#define SYN_REPORT  0u

/* ---- stage allocation --------------------------------------------- */

/* one mmap is capped at 64 pages (256 KiB) by the kernel; the
 * mmap window marches upward page-aligned, so N consecutive
 * mmap_anon(256 KiB) calls yield a CONTIGUOUS region -- the stage
 * is 8 of them, 2 MiB, enough for 800x600 XRGB8888.               */
#define STAGE_CHUNK (64u * 4096u)
#define STAGE_CHUNKS 8u

/* ---- state ----------------------------------------------------------- */

#define MAX_WINS     6           /* one per app; SHM_OBJS_MAX 12  */
#define MAX_CLIENTS  8           /* connections incl. windowless  */
#define MAX_BANNERS  3

struct window {
    int  used;
    u32  id;
    char name[12];
    int  fd;                        /* client socket, -1 = none    */
    int  shm_id;
    u32 *va;                        /* compositor mapping          */
    u32  w, h;
    i32  x, y;
    bool visible;

    /* phase 16 (item 88): dirty-rect accumulation. An app FLUSH
     * no longer triggers a full-screen recompose: the render
     * thread blits only this window's union rect into the stage
     * and presents just that strip. */
    bool has_dirty;
    u32  dx0, dy0, dx1, dy1;
};

struct client {
    int  used;
    int  fd;
    char name[12];
    int  win;                       /* index into wins, -1 = none  */
};

enum ui_mode {
    MODE_LOCK = 0,
    MODE_HOME,
    MODE_APP,
};

static struct ui_surface stage;
static struct fbio_info fb;
static int fbfd = -1, evfd = -1, listen_fd = -1, modem_fd = -1;

static struct window wins[MAX_WINS];
static struct client clients[MAX_CLIENTS];
static u32 next_win = 1;
static int focused = -1;            /* index into wins             */

static enum ui_mode mode = MODE_LOCK;
static char pin[8];                 /* digits entered so far       */
#define PIN "1234"

static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool need_redraw;

/* phase 16 (item 88): per-window dirty bits consumed by the
 * render thread (see struct window + UI_FLUSH)                    */
static volatile u32 win_dirty_mask;

static struct {
    char text[28];
    u32  kind;
    u64  until_ms;
} banners[MAX_BANNERS];
static unsigned unread_sms;

static u32 sig_bars = 4;            /* last SIGNAL query result    */
static char call_state[16] = "IDLE";
static u32 batt_pct, batt_charging;

/* launcher table (item 81): icon label -> program name; row-major
 * order matches include/ui_layout.h's grid and the phase-15
 * selftest's tap sequence                                        */
static const char *const launch_apps[UI_ICON_COLS * UI_ICON_ROWS] = {
    "dialer", "msgs", "contacts",
    "clock", "calc", "settings",
};

/* ---- framebuffer presentation ----------------------------------------- */

/* kernel rejects blits wider than its 4096-byte row buffer; chunk
 * horizontally to stay inside it (800-wide frames are one call)  */
#define BLIT_MAX_W (4096u / 4u)

static void fb_blit(u32 x, u32 y, u32 w, u32 h)
{
    u32 cx = x, left = w;

    while (left && cx < stage.w) {
        struct fbio_blit b;
        u32 cw = left > BLIT_MAX_W ? BLIT_MAX_W : left;

        if (cw > stage.w - cx)
            cw = stage.w - cx;
        memset(&b, 0, sizeof(b));
        b.x = cx;
        b.y = y;
        b.w = cw;
        b.h = h;
        /* src points at the rect's first pixel; the kernel walks
         * packed rows of b.w * 4 bytes from there              */
        b.src = (u64)(uintptr_t)(stage.px +
                                 (u64)y * stage.w + cx);
        ioctl(fbfd, FBIO_BLIT, (u64)(uintptr_t)&b);
        cx += cw;
        left -= cw;
    }
}

static u64 now_ms(void)
{
    return uptime_ms();
}

/* ---- banners ------------------------------------------------------------ */

/* slot for a 4 s banner; caller must hold ui_lock                */
static void push_banner(const char *text, u32 kind)
{
    unsigned slot = MAX_BANNERS - 1;

    for (unsigned i = 0; i < MAX_BANNERS; i++)
        if (!banners[i].until_ms || banners[i].until_ms < now_ms()) {
            slot = i;
            break;
        }
    snprintf(banners[slot].text, sizeof(banners[slot].text),
             "%s", text);
    banners[slot].kind = kind;
    banners[slot].until_ms = now_ms() + 4000u;
    need_redraw = true;
}

/* ---- window plumbing --------------------------------------------------- */

static void drop_window(int idx)
{
    struct window *w = &wins[idx];

    if (!w->used)
        return;
    if (w->va)
        shmdt(w->va);
    w->va = NULL;
    w->used = false;
    w->fd = -1;
    if (focused == idx) {
        focused = -1;
        mode = MODE_HOME;
    }
    need_redraw = true;
}

static struct window *find_win_by_name(const char *name)
{
    for (int i = 0; i < MAX_WINS; i++)
        if (wins[i].used && !strncmp(wins[i].name, name,
                                     sizeof(wins[i].name)))
            return &wins[i];
    return NULL;
}

/* ---- composition --------------------------------------------------------- */

static void draw_status_bar(void)
{
    char buf[16];
    u32 sec, a, b;

    ui_fill(&stage, 0, 0, stage.w, UI_STATUS_H,
            ui_theme_dark.panel);

    /* signal: 4 bars, leftmost shortest                           */
    for (u32 i = 0; i < 4; i++) {
        u32 bh = 4u + i * 3u;

        ui_fill(&stage, 6u + i * 7u,
                UI_STATUS_H - 4u - bh, 4u, bh,
                i < sig_bars ? ui_theme_dark.fg : 0xff3d444du);
    }

    /* call state pill                                             */
    if (call_state[0] && strcmp(call_state, "IDLE")) {
        snprintf(buf, sizeof(buf), "%s", call_state);
        ui_text(&stage, 42, 8, buf, ui_theme_dark.ok,
                ui_theme_dark.panel);
    }

    /* clock: real epoch when it looks sane, uptime else           */
    sec = (u32)(gettime_ns() / 1000000000ull);
    if (sec > 100000000u) {
        a = (sec / 3600u) % 24u;
        b = (sec / 60u) % 60u;
        snprintf(buf, sizeof(buf), "%02u:%02u", a, b);
    } else {
        sec = (u32)(now_ms() / 1000u);
        a = sec / 60u % 100u;
        b = sec % 60u;
        snprintf(buf, sizeof(buf), "%02u:%02u", a, b);
    }
    ui_text(&stage, stage.w / 2u - ui_text_w(buf) / 2u, 8, buf,
            ui_theme_dark.fg, ui_theme_dark.panel);

    /* battery                                                     */
    if (batt_pct) {
        snprintf(buf, sizeof(buf), "%u%%%s", batt_pct,
                 batt_charging ? "+" : "");
        ui_text(&stage, stage.w - 12u - ui_text_w(buf), 8, buf,
                batt_charging ? ui_theme_dark.ok
                              : ui_theme_dark.fg,
                ui_theme_dark.panel);
    }
}

static void draw_home_bar(void)
{
    ui_fill(&stage, 0, stage.h - UI_HOME_H, stage.w, UI_HOME_H,
            ui_theme_dark.panel);
    ui_fill(&stage, stage.w / 2u - 24u, stage.h - UI_HOME_H / 2u
            - 3u, 48u, 6u, ui_theme_dark.accent);
}

static void draw_lock_screen(void)
{
    static const char *const labels[UI_NUMPAD_ROWS][UI_NUMPAD_COLS] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "C", "0", "OK" },
    };
    char dots[9];

    ui_fill(&stage, 0, UI_STATUS_H, stage.w,
            stage.h - UI_STATUS_H, ui_theme_dark.bg);
    ui_text(&stage, stage.w / 2u - ui_text_w("PHONE LOCKED") / 2u,
            90, "PHONE LOCKED", ui_theme_dark.fg, ui_theme_dark.bg);

    for (u32 i = 0; i < 4; i++)
        dots[i] = i < strlen(pin) ? '*' : '-';
    dots[4] = 0;
    ui_text(&stage, stage.w / 2u - ui_text_w(dots) / 2u, 150, dots,
            ui_theme_dark.accent, ui_theme_dark.bg);

    for (u32 r = 0; r < UI_NUMPAD_ROWS; r++)
        for (u32 c = 0; c < UI_NUMPAD_COLS; c++) {
            struct ui_view v = {
                (i32)(UI_NUMPAD_X0 + c * UI_KEY_DX),
                (i32)(UI_NUMPAD_Y0 + r * UI_KEY_DY),
                (i32)UI_KEY_W, (i32)UI_KEY_H, 0, false,
            };

            ui_button(&stage, &v, labels[r][c], false);
        }
}

static void draw_home_screen(void)
{
    ui_fill(&stage, 0, UI_STATUS_H, stage.w,
            stage.h - UI_STATUS_H - UI_HOME_H, ui_theme_dark.bg);
    ui_text(&stage, 12, UI_STATUS_H + 10, "MOBILE PHONE OS",
            0xff6e7681u, ui_theme_dark.bg);

    for (u32 r = 0; r < UI_ICON_ROWS; r++)
        for (u32 c = 0; c < UI_ICON_COLS; c++) {
            u32 idx = r * UI_ICON_COLS + c;
            const char *name = launch_apps[idx];
            char label[16];
            struct ui_view v = {
                (i32)(UI_ICON_X0 + c * UI_ICON_DX),
                (i32)(UI_ICON_Y0 + r * UI_ICON_DY),
                (i32)UI_ICON_W, (i32)UI_ICON_H, 0, false,
            };

            ui_button(&stage, &v, "", false);
            for (unsigned k = 0; k < sizeof(label) - 1 && name[k];
                 k++)
                label[k] = name[k] >= 'a' && name[k] <= 'z'
                           ? (char)(name[k] - 'a' + 'A') : name[k];
            label[strlen(name) > 15 ? 15 : strlen(name)] = 0;
            ui_text(&stage,
                    (u32)v.x + (UI_ICON_W - strlen(name) * 8u) / 2u,
                    (u32)v.y + UI_ICON_H / 2u - 4u, label,
                    ui_theme_dark.btn_text, ui_theme_dark.btn);

            /* unread badge on the Messages icon                */
            if (idx == 1 && unread_sms) {
                char b2[4];

                snprintf(b2, sizeof(b2), "%u",
                         unread_sms > 9u ? 9u : unread_sms);
                ui_fill(&stage, (u32)v.x + UI_ICON_W - 18u,
                        (u32)v.y - 6u, 18u, 16u, 0xffd1242fu);
                ui_text(&stage, (u32)v.x + UI_ICON_W - 14u,
                        (u32)v.y - 4u, b2, 0xffffffffu,
                        0xffd1242fu);
            }
        }
}

/* copy an app surface into the stage (rows are packed, same as
 * the shm window layout)                                         */
static void blit_window(const struct window *w)
{
    u32 cw = w->w, ch = w->h;

    if ((u32)w->x + cw > stage.w)
        cw = stage.w - (u32)w->x;
    if ((u32)w->y + ch > stage.h)
        ch = stage.h - (u32)w->y;
    for (u32 r = 0; r < ch; r++)
        memcpy(stage.px + (u64)(w->y + (i32)r) * stage.w +
                   (u32)w->x,
               w->va + (u64)r * w->w, cw * 4u);
}

static void draw_app_windows(void)
{
    ui_fill(&stage, 0, UI_STATUS_H, stage.w,
            stage.h - UI_STATUS_H - UI_HOME_H, ui_theme_dark.bg);

    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < MAX_WINS; i++) {
            struct window *w = &wins[i];
            struct ui_view title;
            bool is_focused = (i == focused);

            if (!w->used || !w->visible || !w->va)
                continue;
            if ((pass == 0) == is_focused)
                continue;           /* focused window last       */

            blit_window(w);

            title.x = w->x;
            title.y = w->y;
            title.w = (i32)w->w;
            title.h = UI_TITLE_H;
            ui_fill(&stage, (u32)title.x, (u32)title.y,
                    (u32)title.w, UI_TITLE_H,
                    is_focused ? ui_theme_dark.accent
                               : ui_theme_dark.panel);
            ui_text(&stage, (u32)title.x + 4u, (u32)title.y + 6u,
                    w->name, 0xffffffffu, ui_theme_dark.accent);

            if (is_focused) {
                ui_rect(&stage, (u32)w->x - 2u, (u32)w->y - 2u,
                        w->w + 4u, w->h + 4u,
                        ui_theme_dark.accent);
            }
        }
}

static void draw_banners(void)
{
    for (unsigned i = 0; i < MAX_BANNERS; i++) {
        u32 y = UI_STATUS_H + 4u + i * 26u;
        u32 border;

        if (!banners[i].until_ms || banners[i].until_ms < now_ms())
            continue;
        border = banners[i].kind == UI_NOTIFY_SMS
                     ? ui_theme_dark.accent
                     : banners[i].kind == UI_NOTIFY_CALL
                           ? ui_theme_dark.ok
                           : ui_theme_dark.warn;
        ui_fill(&stage, 8u, y, stage.w - 16u, 22u,
                ui_theme_dark.panel);
        ui_rect(&stage, 8u, y, stage.w - 16u, 22u, border);
        ui_text(&stage, 14u, y + 7u, banners[i].text, 0xffffffffu,
                ui_theme_dark.panel);
    }
}

static void redraw_stage(void)
{
    switch (mode) {
    case MODE_LOCK:
        draw_lock_screen();
        break;
    case MODE_HOME:
        draw_home_screen();
        break;
    case MODE_APP:
        draw_app_windows();
        break;
    }
    draw_status_bar();
    if (mode != MODE_LOCK)
        draw_home_bar();
    draw_banners();
}

/* ---- render thread ------------------------------------------------------- */

/* phase 16 (item 88): composite + present one window's dirty
 * strip only -- an app frame costs its own rect, not a full
 * 800x600 pass (the GPU-less fast path)                            */
static void present_window_strip(struct window *w)
{
    u32 dx0 = w->dx0, dy0 = w->dy0;
    u32 dw = w->dx1 - w->dx0, dh = w->dy1 - w->dy0;

    for (u32 r = 0; r < dh; r++)
        memcpy(stage.px + (u64)(w->y + (i32)(dy0 + r)) * stage.w +
                   (u32)w->x + dx0,
               w->va + (u64)(dy0 + r) * w->w + dx0, dw * 4u);
    fb_blit((u32)w->x + dx0, (u32)w->y + dy0, dw, dh);
    w->has_dirty = false;
}

static void *render_thread(void *arg)
{
    u32 last_sec = 0xffffffffu;

    (void)arg;
    for (;;) {
        struct batt_info bi;
        u32 dirty;

        sleep_ms(100);

        batt_pct = 0;
        batt_charging = 0;
        if (battinfo(&bi) == 0 && bi.present) {
            batt_pct = bi.percent;
            batt_charging = bi.current_ma > 0;
        }

        pthread_mutex_lock(&ui_lock);

        /* 1. app frames: per-window strips (item 88 fast path)    */
        dirty = win_dirty_mask;
        win_dirty_mask = 0;
        while (dirty) {
            unsigned i = __builtin_ctz(dirty);

            dirty &= ~(1u << i);
            if (wins[i].used && wins[i].visible && wins[i].va &&
                wins[i].has_dirty)
                present_window_strip(&wins[i]);
        }

        /* 2. full recompose for mode/focus/banner changes         */
        if (need_redraw) {
            redraw_stage();
            fb_blit(0, 0, stage.w, stage.h);
            need_redraw = false;
            last_sec = (u32)(now_ms() / 1000u);
        } else if (last_sec != (u32)(now_ms() / 1000u)) {
            /* 3. clock/battery tick: just the status strip        */
            draw_status_bar();
            fb_blit(0, 0, stage.w, UI_STATUS_H);
            last_sec = (u32)(now_ms() / 1000u);
        }
        pthread_mutex_unlock(&ui_lock);
    }
    return NULL;
}

/* ---- modem thread ----------------------------------------------------------- */

/* rssi 0..31 -> 1..4 bars; 99 ("not known") -> 0                  */
static void set_signal(long rssi)
{
    u32 bars = 0;

    if (rssi >= 0 && rssi < 99)
        bars = (u32)rssi * 4u / 31u;
    if (rssi > 0 && bars == 0)
        bars = 1;
    if (bars != sig_bars) {
        sig_bars = bars;
        need_redraw = true;
    }
}

static void set_call_state(const char *name)
{
    snprintf(call_state, sizeof(call_state), "%s", name);
    need_redraw = true;
}

static void modem_line(char *line)
{
    if (!strncmp(line, "EV SMS ", 7)) {
        char *sp = strchr(line + 7, ' ');
        char buf[28];

        if (sp) {
            *sp = 0;
            snprintf(buf, sizeof(buf), "SMS %s: %s", line + 7,
                     sp + 1);
        } else {
            snprintf(buf, sizeof(buf), "SMS %s", line + 7);
        }
        printf("[ui] banner: %s\n", buf);
        pthread_mutex_lock(&ui_lock);
        unread_sms++;
        push_banner(buf, UI_NOTIFY_SMS);
        pthread_mutex_unlock(&ui_lock);
    } else if (!strncmp(line, "EV CALL ", 8)) {
        const char *name = line + 8;
        static const char *const loud[] = {
            "RING", "INCOMING", "CONNECT", "HANGUP-REMOTE", "BUSY",
        };
        char buf[24];

        printf("[ui] call event: %s\n", name);
        for (unsigned i = 0; i < sizeof(loud) / sizeof(loud[0]);
             i++) {
            if (!strcmp(name, loud[i])) {
                snprintf(buf, sizeof(buf), "CALL %s", name);
                printf("[ui] banner: %s\n", buf);
                pthread_mutex_lock(&ui_lock);
                push_banner(buf, UI_NOTIFY_CALL);
                pthread_mutex_unlock(&ui_lock);
                break;
            }
        }
        pthread_mutex_lock(&ui_lock);
        set_call_state(name);
        pthread_mutex_unlock(&ui_lock);
    } else if (!strncmp(line, "SIG ", 4)) {
        long rssi = 0, ber = 0;
        const char *p = line + 4;
        const char *end;

        rssi = (long)strtoul(p, &end, 10);
        if (end != p)
            ber = (long)strtoul(end, &end, 10);
        (void)ber;
        pthread_mutex_lock(&ui_lock);
        set_signal(rssi);
        pthread_mutex_unlock(&ui_lock);
    } else if (!strncmp(line, "STATE ", 6)) {
        pthread_mutex_lock(&ui_lock);
        set_call_state(line + 6);
        pthread_mutex_unlock(&ui_lock);
    }
}

static void *modem_thread(void *arg)
{
    char line[96];
    unsigned len = 0;
    u64 last_query = 0;

    (void)arg;
    for (;;) {
        if (modem_fd < 0) {
            modem_fd = usock_connect("/var/run/modem");
            if (modem_fd < 0) {
                sleep_ms(2000);
                continue;
            }
            printf("[ui] modem connected\n");
            len = 0;
            write(modem_fd, "STATUS\n", 7);
            last_query = now_ms();
        }

        {
            char c;
            i64 r = read(modem_fd, &c, 1);

            if (r <= 0) {
                close(modem_fd);
                modem_fd = -1;
                continue;
            }
            if (c == '\n' || c == '\r') {
                if (len) {
                    line[len] = 0;
                    modem_line(line);
                    len = 0;
                }
            } else if (len + 1 < sizeof(line)) {
                line[len++] = c;
            }
        }

        /* keep the signal bars honest; replies come back as
         * "SIG <rssi> <ber>" lines above                       */
        if (modem_fd >= 0 && now_ms() - last_query > 5000u) {
            write(modem_fd, "SIGNAL\n", 7);
            last_query = now_ms();
        }
    }
    return NULL;
}

/* ---- client protocol ---------------------------------------------------------- */

static i64 read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t left = n;

    while (left) {
        i64 r = read(fd, p, left);

        if (r <= 0)
            return -1;
        p += r;
        left -= (size_t)r;
    }
    return (i64)n;
}

static void send_msg(int fd, u32 type, u32 win, i32 a, i32 b, i32 c,
                     const char *text)
{
    struct ui_msg m;

    memset(&m, 0, sizeof(m));
    m.type = type;
    m.win = win;
    m.a = a;
    m.b = b;
    m.c = c;
    if (text)
        strncpy(m.text, text, sizeof(m.text) - 1);
    write(fd, &m, UI_MSG_SIZE);
}

/* client thread: owns one connection end to end                  */
static void *client_thread(void *arg)
{
    struct client *cl = arg;

    for (;;) {
        struct ui_msg m;

        if (read_full(cl->fd, &m, UI_MSG_SIZE) < 0)
            break;

        pthread_mutex_lock(&ui_lock);

        switch (m.type) {
        case UI_HELLO:
            snprintf(cl->name, sizeof(cl->name), "%s", m.text);
            printf("[ui] hello: %s\n", cl->name);
            send_msg(cl->fd, UI_WELCOME, 0, (i32)stage.w,
                     (i32)stage.h, 0, NULL);
            break;

        case UI_OPEN: {
            struct window *w = NULL;
            u32 ww = m.a > 0 ? (u32)m.a : UI_WIN_W;
            u32 wh = m.b > 0 ? (u32)m.b : UI_WIN_H;
            u32 npages;
            u32 *va;
            int idx = -1;

            if (ww > stage.w - 8u)
                ww = stage.w - 8u;
            if (wh > stage.h - UI_STATUS_H - UI_HOME_H - 8u)
                wh = stage.h - UI_STATUS_H - UI_HOME_H - 8u;
            npages = (ww * wh * 4u + 4095u) / 4096u;

            for (int i = 0; i < MAX_WINS; i++)
                if (!wins[i].used) {
                    idx = i;
                    break;
                }
            if (idx >= 0) {
                int shm_id = shmget(npages);

                if (shm_id >= 0) {
                    va = shmat(shm_id);
                    if (va && va != (u32 *)-1) {
                        w = &wins[idx];
                        memset(w, 0, sizeof(*w));
                        w->used = true;
                        w->id = next_win++;
                        snprintf(w->name, sizeof(w->name), "%s",
                                 cl->name);
                        w->fd = cl->fd;
                        w->shm_id = shm_id;
                        w->va = va;
                        w->w = ww;
                        w->h = wh;
                        w->x = (i32)((stage.w - ww) / 2u);
                        w->y = (i32)(UI_STATUS_H + 32u);
                    }
                }
            }
            if (w) {
                cl->win = idx;
                printf("[ui] window %s: id %u shm %d at %d,%d "
                       "(%ux%u)\n",
                       w->name, w->id, w->shm_id, w->x, w->y,
                       w->w, w->h);
                send_msg(cl->fd, UI_OPENED, w->id, w->shm_id,
                         w->x, w->y, NULL);
            } else {
                send_msg(cl->fd, UI_CLOSED, m.win, 0, 0, 0, NULL);
            }
            break;
        }

        case UI_FLUSH:
            /*
             * phase 16 (item 88): accumulate the dirty rect in
             * surface coords; the render thread recomposites just
             * this window's strip. Falls back to the full path
             * only when the window is not currently visible.
             */
            if (cl->win >= 0 && wins[cl->win].used) {
                struct window *w = &wins[cl->win];
                u32 x0 = m.a > 0 ? (u32)m.a : 0u;
                u32 y0 = m.b > 0 ? (u32)m.b : 0u;
                u32 x1 = x0 + (u32)(m.c > 0 ? m.c : 0) ;
                u32 y1 = y0 + (u32)(m.d > 0 ? m.d : 0);

                if (x1 > w->w)
                    x1 = w->w;
                if (y1 > w->h)
                    y1 = w->h;
                if (x1 <= x0 || y1 <= y0) {
                    x0 = 0; y0 = 0; x1 = w->w; y1 = w->h;
                }
                if (!w->has_dirty) {
                    w->dx0 = x0; w->dy0 = y0;
                    w->dx1 = x1; w->dy1 = y1;
                    w->has_dirty = true;
                } else {
                    if (x0 < w->dx0) w->dx0 = x0;
                    if (y0 < w->dy0) w->dy0 = y0;
                    if (x1 > w->dx1) w->dx1 = x1;
                    if (y1 > w->dy1) w->dy1 = y1;
                }
                win_dirty_mask |= 1u << cl->win;
            } else {
                need_redraw = true;
            }
            break;

        case UI_SHOW:
            if (cl->win >= 0 && wins[cl->win].used) {
                wins[cl->win].visible = true;
                focused = cl->win;
                mode = MODE_APP;
                send_msg(cl->fd, UI_FOCUS, wins[cl->win].id, 1,
                         0, 0, NULL);
                need_redraw = true;
            }
            break;

        case UI_HIDE:
            if (cl->win >= 0 && wins[cl->win].used) {
                wins[cl->win].visible = false;
                if (focused == cl->win) {
                    focused = -1;
                    mode = MODE_HOME;
                    send_msg(cl->fd, UI_FOCUS,
                             wins[cl->win].id, 0, 0, 0, NULL);
                }
                need_redraw = true;
            }
            break;

        case UI_CLOSE:
            if (cl->win >= 0) {
                drop_window(cl->win);
                cl->win = -1;
            }
            break;

        case UI_NOTIFY: {
            char buf[28];

            snprintf(buf, sizeof(buf), "%s", m.text);
            printf("[ui] banner: %s\n", buf);
            push_banner(buf, (u32)m.a);
            break;
        }

        default:
            break;
        }

        pthread_mutex_unlock(&ui_lock);
    }

    /* connection gone: forget client + window                     */
    pthread_mutex_lock(&ui_lock);
    if (cl->win >= 0)
        drop_window(cl->win);
    close(cl->fd);
    cl->fd = -1;
    cl->used = false;
    printf("[ui] client %s gone\n", cl->name);
    pthread_mutex_unlock(&ui_lock);
    return NULL;
}

static void *server_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = usock_accept(listen_fd);
        struct client *cl = NULL;

        if (fd < 0) {
            sleep_ms(50);
            continue;
        }
        pthread_mutex_lock(&ui_lock);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i].used) {
                cl = &clients[i];
                break;
            }
        if (cl) {
            cl->used = true;
            cl->fd = fd;
            cl->win = -1;
            cl->name[0] = 0;
        }
        pthread_mutex_unlock(&ui_lock);

        if (cl) {
            pthread_t t;

            if (pthread_create(&t, NULL, client_thread, cl)) {
                close(fd);      /* no thread: drop the conn  */
                cl->used = false;
                cl->fd = -1;
            }
        } else {
            close(fd);          /* table full                 */
        }
    }
    return NULL;
}

/* ---- input + chrome taps -------------------------------------------------------- */

static void go_home(void)
{
    focused = -1;
    mode = MODE_HOME;
    need_redraw = true;
}

static void launch_app(const char *name)
{
    /* already running? just raise it                              */
    for (int i = 0; i < MAX_WINS; i++)
        if (wins[i].used && !strncmp(wins[i].name, name,
                                     sizeof(wins[i].name))) {
            wins[i].visible = true;
            focused = i;
            mode = MODE_APP;
            send_msg(wins[i].fd, UI_FOCUS, wins[i].id, 1, 0, 0,
                     NULL);
            need_redraw = true;
            printf("[ui] raise %s\n", name);
            return;
        }

    printf("[ui] launch %s\n", name);
    {
        int pid = fork();

        if (pid == 0) {
            char *const argv[] = { (char *)name, NULL };

            /* the child must not hold the compositor's
             * device + listener + client fds across exec:
             * an unread duplicate modem fd, for example,
             * would fill up and stall modemd's fan-out  */
            close(fbfd);
            close(evfd);
            close(listen_fd);
            if (modem_fd >= 0)
                close(modem_fd);
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].used && clients[i].fd >= 0)
                    close(clients[i].fd);

            if (execve(name, argv, (char *const[]){ 0 }) < 0)
                _exit(127);
            for (;;)
                ;
        }
        (void)pid;              /* init (orphan reaper) reaps     */
    }
}

static bool key_at(i32 x, i32 y, u32 *row, u32 *col)
{
    for (u32 r = 0; r < UI_NUMPAD_ROWS; r++)
        for (u32 c = 0; c < UI_NUMPAD_COLS; c++) {
            i32 kx = (i32)(UI_NUMPAD_X0 + c * UI_KEY_DX);
            i32 ky = (i32)(UI_NUMPAD_Y0 + r * UI_KEY_DY);

            if (x >= kx && x < kx + (i32)UI_KEY_W &&
                y >= ky && y < ky + (i32)UI_KEY_H) {
                *row = r;
                *col = c;
                return true;
            }
        }
    return false;
}

static void tap_lock(i32 x, i32 y)
{
    u32 r, c;

    if (!key_at(x, y, &r, &c))
        return;

    if (r < 3u || (r == 3u && c == 1u)) {
        /* digits: 1..9 row-major, then 0 in the bottom row     */
        char d = r == 3u ? '0' : (char)('1' + (char)(r * 3u + c));

        if (strlen(pin) < 4) {
            pin[strlen(pin)] = d;
            need_redraw = true;
        }
    } else if (c == 0) {        /* C: backspace                  */
        if (strlen(pin))
            pin[strlen(pin) - 1] = 0;
        need_redraw = true;
    } else {                    /* OK                            */
        if (!strcmp(pin, PIN)) {
            printf("[ui] unlock ok\n");
            pin[0] = 0;
            mode = MODE_HOME;
        } else {
            printf("[ui] wrong pin\n");
            pin[0] = 0;
            push_banner("WRONG PIN", UI_NOTIFY_INFO);
        }
        need_redraw = true;
    }
}

static void tap_home(i32 x, i32 y)
{
    for (u32 r = 0; r < UI_ICON_ROWS; r++)
        for (u32 c = 0; c < UI_ICON_COLS; c++) {
            i32 ix = (i32)(UI_ICON_X0 + c * UI_ICON_DX);
            i32 iy = (i32)(UI_ICON_Y0 + r * UI_ICON_DY);

            if (x >= ix && x < ix + (i32)UI_ICON_W &&
                y >= iy && y < iy + (i32)UI_ICON_H) {
                launch_app(launch_apps[r * UI_ICON_COLS + c]);
                return;
            }
        }
}

static void forward_event(int fd, u32 win, u16 type, u16 code,
                          i32 value)
{
    send_msg(fd, UI_EVENT, win, (i32)type, (i32)code, value, NULL);
}

static void tap_app(i32 x, i32 y, bool down)
{
    if (down && y >= (i32)(stage.h - UI_HOME_H)) {
        go_home();
        return;
    }
    if (focused < 0 || !wins[focused].used)
        return;

    {
        struct window *w = &wins[focused];
        i32 rx = x - w->x, ry = y - w->y;

        if (rx < 0 || ry < 0 ||
            rx >= (i32)w->w || ry >= (i32)w->h)
            return;
        /* the window's own coords: ABS X/Y then the touch
         * switch, mirroring the /dev/event0 stream shape      */
        if (down) {
            forward_event(w->fd, w->id, EV_ABS, ABS_X, rx);
            forward_event(w->fd, w->id, EV_ABS, ABS_Y, ry);
        }
        forward_event(w->fd, w->id, EV_KEY, BTN_TOUCH,
                      down ? 1 : 0);
    }
}

static void *input_thread(void *arg)
{
    u32 abs_x, abs_y;
    bool down = false, last_down = false;

    (void)arg;
    for (;;) {
        struct wire_ev ev;

        if (read_full(evfd, &ev, sizeof(ev)) < 0) {
            sleep_ms(100);
            continue;
        }

        switch (ev.type) {
        case EV_ABS:
            if (ev.code == ABS_X)
                abs_x = (u32)ev.value;
            else if (ev.code == ABS_Y)
                abs_y = (u32)ev.value;
            break;

        case EV_KEY:
            if (ev.code == BTN_TOUCH)
                down = ev.value != 0;
            else if (ev.code == KEY_POWER && ev.value == 1) {
                pthread_mutex_lock(&ui_lock);
                pin[0] = 0;
                mode = MODE_LOCK;
                need_redraw = true;
                printf("[ui] power: lock\n");
                pthread_mutex_unlock(&ui_lock);
            }
            break;

        case EV_SYN:
            if (ev.code != SYN_REPORT)
                break;
            if (down == last_down)
                break;

            pthread_mutex_lock(&ui_lock);
            if (down) {
                printf("[ui] tap %u,%u (mode %d)\n", abs_x,
                       abs_y, (int)mode);
                if (mode == MODE_LOCK)
                    tap_lock((i32)abs_x, (i32)abs_y);
                else if (mode == MODE_HOME)
                    tap_home((i32)abs_x, (i32)abs_y);
                else
                    tap_app((i32)abs_x, (i32)abs_y, true);
            } else {
                tap_app((i32)abs_x, (i32)abs_y, false);
            }
            pthread_mutex_unlock(&ui_lock);
            last_down = down;
            break;

        default:
            break;
        }
    }
    return NULL;
}

/* ---- setup + main ------------------------------------------------------------- */

static int stage_alloc(void)
{
    char *first = mmap_anon(STAGE_CHUNK);

    if (!first || first == (char *)-1)
        return -1;
    for (u32 i = 1; i < STAGE_CHUNKS; i++) {
        char *p = mmap_anon(STAGE_CHUNK);

        if (!p || p == (char *)-1)
            return -1;
    }
    stage.px = (u32 *)first;
    stage.w = UI_SCREEN_W;
    stage.h = UI_SCREEN_H;
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    pthread_t t;
    u64 deadline;

    (void)argc;
    (void)argv;
    (void)envp;

    printf("compositor: starting (pid %d)\n", getpid());

    if (stage_alloc()) {
        printf("[ui] stage alloc failed\n");
        return 1;
    }

    /* fb0: the virtio-gpu chardev; canvas arms on first ioctl     */
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) {
        printf("[ui] no /dev/fb0 (%d) -- no display, exiting\n",
               fbfd);
        return 1;
    }
    if (ioctl(fbfd, FBIO_INFO, (u64)(uintptr_t)&fb) ||
        fb.w != UI_SCREEN_W || fb.h != UI_SCREEN_H) {
        printf("[ui] fb0 %ux%u bpp %u unexpected\n", fb.w, fb.h,
               fb.bpp);
        return 1;
    }

    evfd = open("/dev/event0", O_RDONLY);
    if (evfd < 0) {
        printf("[ui] no /dev/event0 (%d) -- exiting\n", evfd);
        return 1;
    }

    mkdir("/var");
    mkdir("/var/run");
    listen_fd = usock_serve("/var/run/ui");
    if (listen_fd < 0) {
        printf("[ui] serve /var/run/ui failed (%d)\n",
               listen_fd);
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].used = false;

    if (pthread_create(&t, NULL, render_thread, NULL) ||
        pthread_create(&t, NULL, server_thread, NULL) ||
        pthread_create(&t, NULL, modem_thread, NULL)) {
        printf("[ui] thread spawn failed\n");
        return 1;
    }

    printf("[ui] compositor ready: %ux%u stage, fb0 + event0, "
           "/var/run/ui\n",
           stage.w, stage.h);

    /* the selftest needs a beat before it starts pushing touches  */
    deadline = now_ms() + 500u;
    while (now_ms() < deadline)
        sleep_ms(50);

    input_thread(NULL);
    return 0;
}
