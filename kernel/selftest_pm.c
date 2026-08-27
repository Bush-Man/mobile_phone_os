/*
 * selftest_pm.c - phase 10 power/battery verification, run as the
 * "pmtest" kernel task.
 *
 * Checks, in order:
 *   1. PSCI conduit: VERSION returns a non-zero BCD version under
 *      QEMU's hvc interception; FEATURES accepts SYSTEM_OFF/RESET.
 *      (Destructive calls are intentionally never invoked here.)
 *   2. Idle governor: WFI counters increase across yields, wake
 *      source table carries at least the timer entry wired in main.
 *   3. Display policy matrix: pm_display_decide() stays consistent
 *      for STAY/SUSPEND/RESUME across synthetic timestamps, then a
 *      real short-timeout suspend + wake-on-touch cycle is executed
 *      live (50 ms blank timeout, injected activity, explicit tick).
 *   4. Battery policy: hysteresis -- WARN fires only on downward
 *      threshold crossing, EXIT_WARN only on recovery beyond the
 *      band edge; CRITICAL always wins below crit line.
 *   5. Mock provider: battery_mock_force() seeds percentages which
 *      battery_poll_tick() picks up; snapshot reaches /dev/battery
 *      data path (string formatter smoke-checked).
 *   6. Milestone line: "battery N% reported" printed from the live
 *      cache at task exit.
 *
 * Summary line "selftest: pm ok" matches the harness grep style.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "battery.h"
#include "chardev.h"
#include "fb.h"
#include "lib.h"
#include "pm.h"
#include "psci.h"
#include "task.h"
#include "time.h"
#include "mm/kheap.h"

static int failures;

#define CHECK(cond, name)                                              \
    do {                                                               \
        if (cond) {                                                    \
            kprintf("pmtest: %-34s ok\n", name);                       \
        } else {                                                       \
            kprintf("pmtest: %-34s FAIL\n", name);                     \
            failures++;                                                \
        }                                                              \
    } while (0)

/* ---- 1: psci ------------------------------------------------------------------------ */

static void psci_tests(void)
{
    uint32_t ver = psci_version();

    CHECK(psci_available(), "psci conduit present");
    CHECK(ver != 0, "psci version query");
    CHECK(psci_has_feature(0x84000008ull), "psci features SYSTEM_OFF");
    CHECK(psci_has_feature(0x84000009ull), "psci features RESET");
}

/* ---- 2: idle governor ----------------------------------------------------------------- */

static void idle_tests(void)
{
    unsigned before = pm_wfi_count();

    msleep(8);                          /* forces two park cycles     */
    msleep(8);

    CHECK(pm_wfi_count() >= before + 1, "wfi accounted in idle path");
    CHECK(pm_wake_source_count() >= 1, "wake sources noted");
    CHECK(pm_select_depth() == PM_IDLE_WFI, "idle depth policy WFI");
}

/* ---- 3: display policy ------------------------------------------------------------------ */

static void display_tests(void)
{
    uint64_t now = time_uptime_ms();
    uint64_t last = now;

    /* live display: fresh activity -> STAY                         */
    CHECK(pm_display_decide(now, last, false, false, 100) ==
              PM_DISP_STAY, "policy fresh-idle STAY");

    /* timeout elapsed -> SUSPEND                                   */
    CHECK(pm_display_decide(now + 101, last, false, false, 100) ==
              PM_DISP_SUSPEND_NOW, "policy timeout SUSPEND");

    /* zero timeout disables blanking                               */
    CHECK(pm_display_decide(now + 99999, last, false, false, 0) ==
              PM_DISP_STAY, "policy timeout-0 stays on");

    /* suspended without pending event: stay dark                   */
    CHECK(pm_display_decide(now, last, true, false, 100) ==
              PM_DISP_STAY, "policy suspended stays dark");

    /* wake on touch: pending flag flips to RESUME                  */
    CHECK(pm_display_decide(now, last, true, true, 100) ==
              PM_DISP_RESUME_NOW, "policy wake RESUME");

    /* ---- live cycle: 50 ms blank, wake on synthetic touch ------- */
    {
        uint32_t old_timeout = pm_display_timeout();
        uint64_t susp_before = pm_suspend_count();
        uint64_t res_before  = pm_resume_count();

        pm_display_set_timeout(50);
        msleep(60);                     /* real short idle window     */
        pm_display_tick(time_uptime_ms());

        CHECK(pm_display_state() == PM_DISP_SUSPENDED ||
                  !fb_present(),          /* no display: engine idle  */
              "display suspended on idle");

        pm_input_activity();            /* the "touch"                */
        pm_display_tick(time_uptime_ms());
        CHECK(pm_display_state() == PM_DISP_ON, "display woke on input");
        CHECK(pm_resume_count() == res_before + 1 ||
                  !fb_present(), "resume counted");

        pm_display_set_timeout(old_timeout);
        (void)susp_before;
    }
}

/* ---- 4+5: battery policy + mock flow ------------------------------------------------ */

static void battery_tests(void)
{
    battery_thresholds_set(20, 7);

    CHECK(battery_policy(50, 50) == BAT_OK, "policy mid OK");
    CHECK(battery_policy(25, 19) == BAT_WARN, "policy WARN crossing");
    CHECK(battery_policy(19, 21) == BAT_OK, "policy in-band quiet");
    CHECK(battery_policy(19, 23) == BAT_EXIT_WARN, "policy recovery");
    CHECK(battery_policy(12, 6) == BAT_CRITICAL, "policy CRITICAL");
    CHECK(battery_policy(8, 9) == BAT_OK, "hysteresis: no flap");

    /* mock flow: force a value, tick, read the snapshot            */
    {
        struct battery_state st;
        struct char_dev *node;
        uint8_t seeded = 66;

        battery_mock_force(seeded);
        CHECK(battery_mock_attached(), "mock provider attached");

        battery_poll_tick(time_uptime_ms() + 5000);
        battery_snapshot_get(&st);
        CHECK(st.present && st.percent == seeded,
              "forced percent reaches snapshot");

        node = char_dev_find("battery");
        if (node) {
            char probe[8];
            int r = node->read(node, probe, sizeof(probe));

            CHECK(r > 0 && probe[0] == 'b', "dev/battery format");
        } else {
            CHECK(false, "dev/battery node lookup");
        }
    }

    {
        struct battery_state st;

        battery_mock_force(73);
        battery_poll_tick(time_uptime_ms() + 5000);
        battery_snapshot_get(&st);
        CHECK(st.present && st.percent == 73 && st.voltage_mv >= 3300u,
              "mock fields consistent");
    }
}

/* ---- milestone line ----------------------------------------------------------------- */

static void milestone_report(void)
{
    struct battery_state st;

    if (battery_snapshot_get(&st) && st.present) {
        kprintf("battery %u%% reported (%s)\n",
                st.percent, battery_charger_hint(st.current_ma));
    } else {
        kprintf("battery: no reading (provider absent)\n");
    }
}

/* ---- entry ------------------------------------------------------------------------ */

void pm_selftest_task(void *arg)
{
    (void)arg;

    kprintf("pmtest: phase 10 power/battery selftests\n");

    psci_tests();
    idle_tests();
    display_tests();
    battery_tests();
    milestone_report();

    if (!failures)
        kprintf("selftest: pm ok\n");
    else
        kprintf("selftest: pm FAILED (%d)\n", failures);

    task_exit();
}

