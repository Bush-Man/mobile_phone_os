/*
 * clock.c - the clock app (phase 15, plan item 82).
 *
 * Shows the wall clock (epoch-based once a source exists, uptime
 * based in the QEMU dev image where epoch stays 0 -- same policy
 * as the compositor status bar) plus session stats. Redraws on a
 * 2 Hz ticker thread; draws are mutex-guarded because the UI
 * thread also repaints on focus events.
 */

#include "libc.h"
#include "ui.h"

static struct ui_client ui;
static pthread_mutex_t draw_lock = PTHREAD_MUTEX_INITIALIZER;

static void clock_parts(u32 *h, u32 *m, u32 *s, bool *wall)
{
    u64 sec = gettime_ns() / 1000000000ull;

    if (sec > 100000000ull) {
        *h = (u32)(sec / 3600ull) % 24u;
        *m = (u32)(sec / 60ull) % 60u;
        *s = (u32)(sec % 60ull);
        *wall = true;
    } else {
        sec = uptime_ms() / 1000ull;
        *h = (u32)(sec / 3600ull) % 100u;
        *m = (u32)(sec / 60ull) % 60u;
        *s = (u32)(sec % 60ull);
        *wall = false;
    }
}

static void draw(void)
{
    u32 h, m, s;
    bool wall;
    char buf[32];
    char sub[40];

    clock_parts(&h, &m, &s, &wall);

    ui_fill(&ui.surf, 0, 0, ui.surf.w, ui.surf.h,
            ui_theme_dark.bg);

    ui_fill(&ui.surf, 40, 80, ui.surf.w - 80, 40,
            ui_theme_dark.panel);
    ui_rect(&ui.surf, 40, 80, ui.surf.w - 80, 40,
            ui_theme_dark.border);
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
    ui_text(&ui.surf,
            (ui.surf.w - ui_text_w(buf)) / 2u, 96, buf,
            ui_theme_dark.accent, ui_theme_dark.panel);

    snprintf(sub, sizeof(sub), "%s (no NTP yet)",
             wall ? "WALL CLOCK" : "UPTIME CLOCK");
    ui_text(&ui.surf,
            (ui.surf.w - ui_text_w(sub)) / 2u, 140, sub,
            ui_theme_dark.fg, ui_theme_dark.bg);

    snprintf(sub, sizeof(sub), "UP %llu MS",
             (unsigned long long)uptime_ms());
    ui_text(&ui.surf,
            (ui.surf.w - ui_text_w(sub)) / 2u, 170, sub,
            0xff6e7681u, ui_theme_dark.bg);

    ui_flush_all(&ui);
}

static void *ticker(void *arg)
{
    (void)arg;
    for (;;) {
        sleep_ms(500);
        pthread_mutex_lock(&draw_lock);
        draw();
        pthread_mutex_unlock(&draw_lock);
    }
    return NULL;
}

int main(int argc, char **argv, char **envp)
{
    pthread_t t;

    (void)argc;
    (void)argv;
    (void)envp;

    if (ui_connect(&ui, "clock", UI_WIN_W, UI_WIN_H) < 0) {
        printf("[clock] no compositor\n");
        return 1;
    }
    printf("[clock] ready (window %u)\n", ui.win);

    if (pthread_create(&t, NULL, ticker, NULL)) {
        printf("[clock] thread failed\n");
        return 1;
    }

    for (;;) {
        struct ui_msg m;
        int ty = ui_next(&ui, &m);

        if (ty < 0) {
            printf("[clock] compositor gone\n");
            return 1;
        }
        if (ty == UI_FOCUS && m.a == 1) {
            pthread_mutex_lock(&draw_lock);
            draw();
            pthread_mutex_unlock(&draw_lock);
        } else if (ty == UI_CLOSED) {
            return 0;
        }
    }
}
