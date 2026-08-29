/*
 * uitest.c - the phase-15 protocol battery (userspace half).
 *
 * Spawned by the kernel "uitest15" task (kernel/selftest_ui.c):
 * connects to the compositor as a window client, proves the
 * HELLO/WELCOME -> OPEN/OPENED -> shm map -> SHOW -> FOCUS ->
 * NOTIFY chain, draws a test pattern into its window and exits 0
 * only when every step answered. A watchdog thread kills the
 * process with exit 1 if the protocol stalls.
 */

#include "libc.h"
#include "ui.h"

static struct ui_client ui;

static void draw_pattern(void)
{
    ui_fill(&ui.surf, 0, 0, ui.surf.w, ui.surf.h,
            ui_theme_dark.bg);
    for (u32 y = 0; y < ui.surf.h; y += 24)
        ui_fill(&ui.surf, 0, y, ui.surf.w, 2,
                ui_theme_dark.panel);
    ui_fill(&ui.surf, 20, 40, ui.surf.w - 40, 60,
            ui_theme_dark.accent);
    ui_text(&ui.surf,
            (ui.surf.w - ui_text_w("UITEST LIVE")) / 2u, 64,
            "UITEST LIVE", 0xffffffffu, ui_theme_dark.accent);
    ui_text(&ui.surf, 20, 130, "PHASE 15 PROTOCOL OK",
            ui_theme_dark.fg, ui_theme_dark.bg);
    ui_flush_all(&ui);
}

static void *watchdog(void *arg)
{
    (void)arg;
    sleep_ms(8000);
    printf("[uitest] TIMEOUT waiting for compositor\n");
    _exit(1);
    return NULL;
}

int main(int argc, char **argv, char **envp)
{
    pthread_t t;
    bool focused = false;

    (void)argc;
    (void)argv;
    (void)envp;

    if (pthread_create(&t, NULL, watchdog, NULL))
        printf("[uitest] watchdog unavailable\n");

    if (ui_connect(&ui, "uitest", 320, 240) < 0) {
        printf("[uitest] FAIL: no compositor\n");
        return 2;
    }
    if (!ui.win || !ui.surf.px) {
        printf("[uitest] FAIL: bad window (win %u)\n", ui.win);
        return 2;
    }
    printf("[uitest] opened win %u shm %d at %d,%d\n", ui.win,
           ui.shm_id, ui.x, ui.y);

    draw_pattern();

    if (ui_show(&ui)) {
        printf("[uitest] FAIL: show\n");
        return 2;
    }

    for (;;) {
        struct ui_msg m;
        int ty = ui_next(&ui, &m);

        if (ty < 0) {
            printf("[uitest] FAIL: compositor gone\n");
            return 2;
        }
        if (ty == UI_FOCUS && m.a == 1) {
            focused = true;
            break;
        }
        if (ty == UI_CLOSED) {
            printf("[uitest] FAIL: window closed\n");
            return 2;
        }
    }
    if (!focused) {
        printf("[uitest] FAIL: no focus\n");
        return 2;
    }

    if (ui_notify(&ui, "UITEST OK", UI_NOTIFY_INFO)) {
        printf("[uitest] FAIL: notify\n");
        return 2;
    }

    printf("[uitest] protocol ok\n");
    _exit(0);
}
