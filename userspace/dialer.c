/*
 * dialer.c - the phone app (phase 15, plan item 82).
 *
 * A ui_client window with a number pad: digits feed the display,
 * CALL dials over the /var/run/modem line protocol, END hangs up,
 * DEL backspaces. A reader thread owns the modem connection's
 * inbound side (OK/ERR replies and EV CALL fan-out lines) and
 * keeps the status strip updated; the main thread only writes
 * requests, so replies and events can never interleave mid-line.
 */

#include "libc.h"
#include "ui.h"

#define MODEM_PATH "/var/run/modem"

static struct ui_client ui;
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static char status[40] = "MODEM: CONNECTING";
static volatile int modem_fd = -1;

/* ---- keypad geometry (app-local; window is 480x360) ---------- */

#define KD_X0  16
#define KD_Y0  60
#define KD_W   100
#define KD_H   48
#define KD_DX  108
#define KD_DY  56

static char number[20];

static void set_status(const char *s)
{
    pthread_mutex_lock(&st_lock);
    snprintf(status, sizeof(status), "%s", s);
    pthread_mutex_unlock(&st_lock);
}

/* ---- modem reader ------------------------------------------------ */

static void *modem_thread(void *arg)
{
    char line[64];
    unsigned len = 0;

    (void)arg;
    for (;;) {
        int fd = usock_connect(MODEM_PATH);
        i64 r;

        if (fd < 0) {
            sleep_ms(1000);
            continue;
        }
        modem_fd = fd;
        set_status("MODEM: READY");

        for (;;) {
            char c;

            r = read(fd, &c, 1);
            if (r <= 0)
                break;
            if (c == '\n' || c == '\r') {
                if (len) {
                    line[len] = 0;
                    len = 0;
                    if (!strncmp(line, "EV CALL ", 8))
                        set_status(line + 8);
                    else if (!strcmp(line, "OK"))
                        set_status("OK");
                    else if (!strncmp(line, "ERR ", 4))
                        set_status(line);
                }
            } else if (len + 1 < sizeof(line)) {
                line[len++] = c;
            }
        }
        modem_fd = -1;
        set_status("MODEM: LOST");
    }
    return NULL;
}

/* ---- drawing -------------------------------------------------------- */

struct dkey {
    i32 x, y, w, h;
    const char *lab;
};

static const struct dkey keys[] = {
    { KD_X0,                KD_Y0,                KD_W, KD_H, "1" },
    { KD_X0 + KD_DX,        KD_Y0,                KD_W, KD_H, "2" },
    { KD_X0 + 2 * KD_DX,    KD_Y0,                KD_W, KD_H, "3" },
    { KD_X0,                KD_Y0 + KD_DY,        KD_W, KD_H, "4" },
    { KD_X0 + KD_DX,        KD_Y0 + KD_DY,        KD_W, KD_H, "5" },
    { KD_X0 + 2 * KD_DX,    KD_Y0 + KD_DY,        KD_W, KD_H, "6" },
    { KD_X0,                KD_Y0 + 2 * KD_DY,    KD_W, KD_H, "7" },
    { KD_X0 + KD_DX,        KD_Y0 + 2 * KD_DY,    KD_W, KD_H, "8" },
    { KD_X0 + 2 * KD_DX,    KD_Y0 + 2 * KD_DY,    KD_W, KD_H, "9" },
    { KD_X0,                KD_Y0 + 3 * KD_DY,    KD_W, KD_H, "*" },
    { KD_X0 + KD_DX,        KD_Y0 + 3 * KD_DY,    KD_W, KD_H, "0" },
    { KD_X0 + 2 * KD_DX,    KD_Y0 + 3 * KD_DY,    KD_W, KD_H, "#" },
};
#define NKEYS ((int)(sizeof(keys) / sizeof(keys[0])))

static const struct dkey btn_call = { 344, 60, 128, 48, "CALL" };
static const struct dkey btn_del  = { 344, 116, 128, 48, "DEL" };
static const struct dkey btn_end  = { 344, 172, 128, 48, "END" };

static void draw(void)
{
    char st[40];

    ui_fill(&ui.surf, 0, 0, ui.surf.w, ui.surf.h,
            ui_theme_dark.bg);

    /* number display                                              */
    ui_fill(&ui.surf, 8, 8, ui.surf.w - 16, 40,
            ui_theme_dark.panel);
    ui_rect(&ui.surf, 8, 8, ui.surf.w - 16, 40,
            ui_theme_dark.border);
    ui_text(&ui.surf, 16, 24,
            number[0] ? number : "ENTER NUMBER",
            ui_theme_dark.fg, ui_theme_dark.panel);

    for (int i = 0; i < NKEYS; i++)
        ui_button(&ui.surf,
                  &(struct ui_view){ keys[i].x, keys[i].y,
                                     keys[i].w, keys[i].h, 0,
                                     false },
                  keys[i].lab, false);

    {
        struct ui_view v = { btn_call.x, btn_call.y, btn_call.w,
                             btn_call.h, 0, false };

        ui_button(&ui.surf, &v, "CALL", false);
        ui_rect(&ui.surf, btn_call.x, btn_call.y, btn_call.w,
                btn_call.h, ui_theme_dark.ok);
    }
    {
        struct ui_view v = { btn_end.x, btn_end.y, btn_end.w,
                             btn_end.h, 0, false };

        ui_button(&ui.surf, &v, "END", false);
        ui_rect(&ui.surf, btn_end.x, btn_end.y, btn_end.w,
                btn_end.h, 0xffd1242fu);
    }
    ui_button(&ui.surf,
              &(struct ui_view){ btn_del.x, btn_del.y, btn_del.w,
                                 btn_del.h, 0, false },
              "DEL", false);

    pthread_mutex_lock(&st_lock);
    snprintf(st, sizeof(st), "%s", status);
    pthread_mutex_unlock(&st_lock);
    ui_fill(&ui.surf, 8, 300, ui.surf.w - 16, 20,
            ui_theme_dark.panel);
    ui_text(&ui.surf, 14, 306, st, ui_theme_dark.warn,
            ui_theme_dark.panel);

    ui_flush_all(&ui);
}

/* ---- taps ------------------------------------------------------------- */

static void do_dial(void)
{
    char req[48];

    if (!number[0]) {
        set_status("NO NUMBER");
        return;
    }
    snprintf(req, sizeof(req), "DIAL %s", number);
    printf("[dialer] dial %s\n", number);
    set_status("DIALING...");
    {
        int fd = modem_fd;

        if (fd >= 0)
            write(fd, req, strlen(req));
    }
}

static void do_hangup(void)
{
    printf("[dialer] hangup\n");
    {
        int fd = modem_fd;

        if (fd >= 0)
            write(fd, "HANGUP", 6);
    }
}

static void tap(i32 x, i32 y)
{
    for (int i = 0; i < NKEYS; i++) {
        if (x >= keys[i].x && x < keys[i].x + keys[i].w &&
            y >= keys[i].y && y < keys[i].y + keys[i].h) {
            size_t n = strlen(number);

            if (n + 1 < sizeof(number)) {
                number[n] = keys[i].lab[0];
                number[n + 1] = 0;
            }
            return;
        }
    }
    if (x >= btn_call.x && x < btn_call.x + btn_call.w &&
        y >= btn_call.y && y < btn_call.y + btn_call.h) {
        do_dial();
        return;
    }
    if (x >= btn_del.x && x < btn_del.x + btn_del.w &&
        y >= btn_del.y && y < btn_del.y + btn_del.h) {
        size_t n = strlen(number);

        if (n)
            number[n - 1] = 0;
        return;
    }
    if (x >= btn_end.x && x < btn_end.x + btn_end.w &&
        y >= btn_end.y && y < btn_end.y + btn_end.h)
        do_hangup();
}

int main(int argc, char **argv, char **envp)
{
    pthread_t t;
    i32 ex = -1, ey = -1;

    (void)argc;
    (void)argv;
    (void)envp;

    if (ui_connect(&ui, "dialer", UI_WIN_W, UI_WIN_H) < 0) {
        printf("[dialer] no compositor\n");
        return 1;
    }
    printf("[dialer] ready (window %u)\n", ui.win);

    if (pthread_create(&t, NULL, modem_thread, NULL)) {
        printf("[dialer] thread failed\n");
        return 1;
    }

    draw();

    for (;;) {
        struct ui_msg m;
        int ty = ui_next(&ui, &m);

        if (ty < 0) {
            printf("[dialer] compositor gone\n");
            return 1;
        }
        if (ty == UI_EVENT) {
            if (m.a == (i32)UI_EV_ABS && m.b == (i32)UI_ABS_X)
                ex = m.c;
            else if (m.a == (i32)UI_EV_ABS && m.b == (i32)UI_ABS_Y)
                ey = m.c;
            else if (m.a == (i32)UI_EV_KEY &&
                     m.b == (i32)UI_BTN_TOUCH &&
                     m.c == 1 && ex >= 0 && ey >= 0)
                tap(ex, ey);
            draw();
        } else if (ty == UI_FOCUS && m.a == 1) {
            draw();
        } else if (ty == UI_CLOSED) {
            return 0;
        }
    }
}
