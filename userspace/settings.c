/*
 * settings.c - the settings app (phase 15, plan item 82).
 *
 * Page-per-subsystem report surface: Battery (SYS_battinfo),
 * Display (compositor geometry), Network (SYS_netinfo), Modem
 * (live /var/run/modem STATUS/SIGNAL state from the reader
 * thread) and About. Wifi/BT rows are listed as N/A: the QEMU dev
 * image has no such transports yet.
 */

#include "libc.h"
#include "sysinfo.h"
#include "ui.h"

#define MODEM_PATH "/var/run/modem"

static struct ui_client ui;
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static char modem_state[16] = "?";
static long modem_rssi = -1;

static const char *const pages[] = {
    "BATTERY", "DISPLAY", "NETWORK", "MODEM", "ABOUT",
};
#define NPAGES ((int)(sizeof(pages) / sizeof(pages[0])))

static int page;

/* ---- modem reader ------------------------------------------------- */

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
        write(fd, "STATUS", 6);
        for (;;) {
            char c;

            r = read(fd, &c, 1);
            if (r <= 0)
                break;
            if (c == '\n' || c == '\r') {
                if (len) {
                    line[len] = 0;
                    len = 0;
                    pthread_mutex_lock(&st_lock);
                    if (!strncmp(line, "STATE ", 6))
                        snprintf(modem_state,
                                 sizeof(modem_state), "%s",
                                 line + 6);
                    else if (!strncmp(line, "SIG ", 4))
                        modem_rssi =
                            (long)strtoul(line + 4, NULL, 10);
                    pthread_mutex_unlock(&st_lock);
                }
            } else if (len + 1 < sizeof(line)) {
                line[len++] = c;
            }
        }
    }
    return NULL;
}

/* ---- drawing ---------------------------------------------------------- */

static void linef(unsigned row, const char *a, long b,
                  const char *unit)
{
    char buf[56];

    snprintf(buf, sizeof(buf), "%s %ld%s", a, b, unit);
    ui_text(&ui.surf, 166, 16 + row * 16, buf,
            ui_theme_dark.fg, ui_theme_dark.bg);
}

static void draw_info(void)
{
    ui_fill(&ui.surf, 160, 8, ui.surf.w - 168, 250,
            ui_theme_dark.panel);
    ui_rect(&ui.surf, 160, 8, ui.surf.w - 168, 250,
            ui_theme_dark.border);
    ui_text(&ui.surf, 168, 16, pages[page], ui_theme_dark.accent,
            ui_theme_dark.panel);

    switch (page) {
    case 0: {
        struct batt_info bi;

        if (battinfo(&bi) == 0 && bi.present) {
            linef(2, "CHARGE %", bi.percent, "");
            linef(3, "VOLTAGE MV", bi.voltage_mv, "");
            linef(4, "CURRENT MA", bi.current_ma,
                  bi.current_ma > 0 ? " (CHG)" : "");
            linef(5, "TEMP DC10", bi.temp_deci_c, "");
            linef(6, "MOCK", bi.is_mock, "");
        } else {
            ui_text(&ui.surf, 168, 48, "NO GAUGE",
                    ui_theme_dark.warn, ui_theme_dark.panel);
        }
        break;
    }
    case 1:
        linef(2, "RESOLUTION X", UI_SCREEN_W, "");
        linef(3, "RESOLUTION Y", UI_SCREEN_H, "");
        linef(4, "BITS PER PIXEL", 32, "");
        linef(5, "WINDOWS MAX", 6, "");
        break;
    case 2: {
        struct netif_info ents[8];
        int n = netinfo(ents, 8);

        if (n <= 0) {
            ui_text(&ui.surf, 168, 48, "NO NETIFS",
                    ui_theme_dark.warn, ui_theme_dark.panel);
            break;
        }
        for (int i = 0; i < n && i < 8; i++) {
            char buf[56];

            snprintf(buf, sizeof(buf), "%s %s %u.%u.%u.%u",
                     ents[i].name,
                     ents[i].up ? "UP" : "DOWN",
                     ents[i].ip & 0xff, (ents[i].ip >> 8) & 0xff,
                     (ents[i].ip >> 16) & 0xff,
                     (ents[i].ip >> 24) & 0xff);
            ui_text(&ui.surf, 168, 48 + (unsigned)i * 16, buf,
                    ui_theme_dark.fg, ui_theme_dark.panel);
        }
        break;
    }
    case 3: {
        char buf[56];

        pthread_mutex_lock(&st_lock);
        snprintf(buf, sizeof(buf), "CALL STATE %s", modem_state);
        ui_text(&ui.surf, 168, 48, buf, ui_theme_dark.fg,
                ui_theme_dark.panel);
        snprintf(buf, sizeof(buf), "RSSI %ld", modem_rssi);
        ui_text(&ui.surf, 168, 64, buf, ui_theme_dark.fg,
                ui_theme_dark.panel);
        pthread_mutex_unlock(&st_lock);
        ui_text(&ui.surf, 168, 96, "WIFI N/A  BT N/A",
                0xff6e7681u, ui_theme_dark.panel);
        break;
    }
    default:
        linef(2, "PHASE", 15, "");
        linef(3, "PID", getpid(), "");
        linef(4, "UPTIME S",
              (long)(uptime_ms() / 1000ull), "");
        break;
    }
}

static void draw(void)
{
    ui_fill(&ui.surf, 0, 0, ui.surf.w, ui.surf.h,
            ui_theme_dark.bg);
    ui_text(&ui.surf, 8, 0, "SETTINGS", ui_theme_dark.fg,
            ui_theme_dark.bg);

    {
        char *items[NPAGES];

        for (int i = 0; i < NPAGES; i++)
            items[i] = (char *)pages[i];
        ui_list(&ui.surf,
                &(struct ui_view){ 8, 12, 140, 190, 0, false },
                items, NPAGES, (unsigned)page);
    }

    draw_info();
    ui_flush_all(&ui);
}

static void tap(i32 x, i32 y)
{
    if (x >= 8 && x < 148 && y >= 12 && y < 202) {
        int row = (y - 14) / 16;

        if (row >= 0 && row < NPAGES) {
            page = row;
            printf("[settings] page %s\n", pages[page]);
        }
    }
}

int main(int argc, char **argv, char **envp)
{
    pthread_t t;
    i32 ex = -1, ey = -1;

    (void)argc;
    (void)argv;
    (void)envp;

    if (ui_connect(&ui, "settings", UI_WIN_W, UI_WIN_H) < 0) {
        printf("[settings] no compositor\n");
        return 1;
    }
    printf("[settings] ready (window %u)\n", ui.win);

    if (pthread_create(&t, NULL, modem_thread, NULL)) {
        printf("[settings] thread failed\n");
        return 1;
    }

    draw();

    for (;;) {
        struct ui_msg m;
        int ty = ui_next(&ui, &m);

        if (ty < 0) {
            printf("[settings] compositor gone\n");
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
