/*
 * msgs.c - SMS/Messaging (phase 15, plan item 82).
 *
 * Reads the kernel SMS store the phase-12 modem layer wrote under
 * /sms/msg<N> ("FROM <sender>\n<text>"), appends anything that
 * arrives live over the /var/run/modem event fan-out, and sends
 * with "SMS <num> <text>". Compose uses the phase-15 on-screen
 * keyboard component (item 84); a reader thread owns the modem
 * connection's inbound side.
 */

#include "libc.h"
#include "ui.h"

#define MODEM_PATH "/var/run/modem"
#define SMS_DB_MAX 12

struct smsg {
    char from[16];
    char text[24];
};

static struct ui_client ui;
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;

static struct smsg msgs[SMS_DB_MAX];
static unsigned nmsgs;
static char recv_field[16], text_field[24];
static int field;                       /* 0 none, 1 recv, 2 text */
static char status[28] = "MODEM: CONNECTING";
static volatile int modem_fd = -1;

/* ---- store load ------------------------------------------------------ */

static void load_store(void)
{
    for (unsigned n = 1; n <= SMS_DB_MAX; n++) {
        char path[16], buf[96];
        int fd;
        i64 r;

        snprintf(path, sizeof(path), "/sms/msg%u", n);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            break;
        r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r <= 0)
            break;
        buf[r] = 0;

        if (!strncmp(buf, "FROM ", 5) && nmsgs < SMS_DB_MAX) {
            char *nl = strchr(buf + 5, '\n');
            struct smsg *m = &msgs[nmsgs];

            if (!nl)
                continue;
            *nl = 0;
            snprintf(m->from, sizeof(m->from), "%s", buf + 5);
            snprintf(m->text, sizeof(m->text), "%s", nl + 1);
            nmsgs++;
        }
    }
    printf("[msgs] loaded %u from /sms store\n", nmsgs);
}

static void add_msg(const char *from, const char *text)
{
    pthread_mutex_lock(&st_lock);
    if (nmsgs == SMS_DB_MAX) {
        memmove(msgs, msgs + 1, sizeof(msgs) - sizeof(msgs[0]));
        nmsgs--;
    }
    snprintf(msgs[nmsgs].from, sizeof(msgs[nmsgs].from), "%s",
             from);
    snprintf(msgs[nmsgs].text, sizeof(msgs[nmsgs].text), "%s",
             text);
    nmsgs++;
    pthread_mutex_unlock(&st_lock);
}

/* ---- modem reader ------------------------------------------------------ */

static void *modem_thread(void *arg)
{
    char line[96];
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
        pthread_mutex_lock(&st_lock);
        snprintf(status, sizeof(status), "MODEM: READY");
        pthread_mutex_unlock(&st_lock);

        for (;;) {
            char c;

            r = read(fd, &c, 1);
            if (r <= 0)
                break;
            if (c == '\n' || c == '\r') {
                if (len) {
                    line[len] = 0;
                    len = 0;
                    if (!strncmp(line, "EV SMS ", 7)) {
                        char *sp = strchr(line + 7, ' ');

                        if (sp) {
                            *sp = 0;
                            add_msg(line + 7, sp + 1);
                            pthread_mutex_lock(&st_lock);
                            snprintf(status, sizeof(status),
                                     "NEW SMS FROM %s", line + 7);
                            pthread_mutex_unlock(&st_lock);
                        }
                    } else if (!strncmp(line, "ERR ", 4)) {
                        pthread_mutex_lock(&st_lock);
                        snprintf(status, sizeof(status), "%s",
                                 line);
                        pthread_mutex_unlock(&st_lock);
                    } else if (!strcmp(line, "OK")) {
                        pthread_mutex_lock(&st_lock);
                        snprintf(status, sizeof(status),
                                 "SENT OK");
                        pthread_mutex_unlock(&st_lock);
                    }
                }
            } else if (len + 1 < sizeof(line)) {
                line[len++] = c;
            }
        }
        modem_fd = -1;
    }
    return NULL;
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_field(i32 x, i32 y, i32 w, const char *txt,
                       bool focus)
{
    ui_fill(&ui.surf, (u32)x, (u32)y, (u32)w, 24,
            ui_theme_dark.panel);
    ui_rect(&ui.surf, (u32)x, (u32)y, (u32)w, 24,
            focus ? ui_theme_dark.accent : ui_theme_dark.border);
    ui_text(&ui.surf, (u32)x + 4, (u32)y + 8,
            txt[0] ? txt : "...",
            focus ? ui_theme_dark.accent : ui_theme_dark.fg,
            ui_theme_dark.panel);
}

static void draw(void)
{
    char st[28];

    ui_fill(&ui.surf, 0, 0, ui.surf.w, ui.surf.h,
            ui_theme_dark.bg);

    ui_text(&ui.surf, 8, 0, "MESSAGES", ui_theme_dark.fg,
            ui_theme_dark.bg);
    for (unsigned i = 0; i < nmsgs && i < 8u; i++) {
        char row[58];
        u32 y = 12u + i * 16u;

        snprintf(row, sizeof(row), "%s: %s", msgs[i].from,
                 msgs[i].text);
        ui_fill(&ui.surf, 8, y, ui.surf.w - 16, 15,
                ui_theme_dark.panel);
        ui_text(&ui.surf, 12, y + 4, row,
                i ? ui_theme_dark.fg : ui_theme_dark.accent,
                ui_theme_dark.panel);
    }

    draw_field(8, 156, 200, recv_field, field == 1);
    draw_field(216, 156, 178, text_field, field == 2);
    ui_button(&ui.surf,
              &(struct ui_view){ 402, 156, 70, 24, 0, false },
              "SEND", false);

    pthread_mutex_lock(&st_lock);
    snprintf(st, sizeof(st), "%s", status);
    pthread_mutex_unlock(&st_lock);
    ui_text(&ui.surf, 8, 186, st, ui_theme_dark.warn,
            ui_theme_dark.bg);

    {
        struct ui_kbd k;

        ui_kbd_init(&k, 0, 210, (i32)ui.surf.w, 140);
        ui_kbd_draw(&ui.surf, &k);
    }

    ui_flush_all(&ui);
}

/* ---- taps --------------------------------------------------------------- */

static void kbd_char(char c)
{
    char *f = field == 1 ? recv_field : text_field;
    size_t cap = field == 1 ? sizeof(recv_field)
                            : sizeof(text_field);
    size_t n = strlen(f);

    if (!field)
        return;
    if (c == '\x08') {
        if (n)
            f[n - 1] = 0;
    } else if (n + 1 < cap) {
        f[n] = c;
        f[n + 1] = 0;
    }
}

static void do_send(void)
{
    char req[64];

    if (!recv_field[0] || !text_field[0])
        return;
    snprintf(req, sizeof(req), "SMS %s %s", recv_field,
             text_field);
    printf("[msgs] send to %s: %s\n", recv_field, text_field);
    {
        int fd = modem_fd;

        if (fd >= 0)
            write(fd, req, strlen(req));
    }
    text_field[0] = 0;
}

static void tap(i32 x, i32 y)
{
    struct ui_kbd k;
    char ch;

    ui_kbd_init(&k, 0, 210, (i32)ui.surf.w, 140);
    if (ui_kbd_tap(&k, x, y, &ch)) {
        kbd_char(ch);
        return;
    }

    if (y >= 156 && y < 180) {
        if (x >= 8 && x < 208)
            field = 1;
        else if (x >= 216 && x < 394)
            field = 2;
        else if (x >= 402 && x < 472)
            do_send();
    }
}

int main(int argc, char **argv, char **envp)
{
    pthread_t t;
    i32 ex = -1, ey = -1;

    (void)argc;
    (void)argv;
    (void)envp;

    if (ui_connect(&ui, "msgs", UI_WIN_W, UI_WIN_H) < 0) {
        printf("[msgs] no compositor\n");
        return 1;
    }
    printf("[msgs] ready (window %u)\n", ui.win);
    load_store();

    if (pthread_create(&t, NULL, modem_thread, NULL)) {
        printf("[msgs] thread failed\n");
        return 1;
    }

    draw();

    for (;;) {
        struct ui_msg m;
        int ty = ui_next(&ui, &m);

        if (ty < 0) {
            printf("[msgs] compositor gone\n");
            return 1;
        }
        if (ty == UI_EVENT) {
            if (m.a == (i32)UI_EV_ABS && m.b == (i32)UI_ABS_X)
                ex = m.c;
            else if (m.a == (i32)UI_EV_ABS &&
                     m.b == (i32)UI_ABS_Y)
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
