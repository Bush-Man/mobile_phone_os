/*
 * batteryd.c - battery status daemon (phase 14, plan item 77).
 *
 * Polls the battery snapshot every 5 s through SYS_battinfo and
 * prints one status line. Its real job in this phase's milestone is
 * to DIE ON PURPOSE under test: the kernel's usertest battery kills
 * it with SIGKILL and init must respawn it -- the visible proof of
 * watchdog-style auto-restart (plan item 78).
 */

#include "libc.h"
#include "sysinfo.h"

int main(int argc, char **argv)
{
    struct batt_info bi;

    (void)argc;
    (void)argv;

    if (battinfo(&bi) == 0 && bi.present)
        printf("batteryd: online (pid %d), %u%% %dmV\n",
               getpid(), bi.percent, bi.voltage_mv);
    else
        printf("batteryd: online (pid %d), no gauge yet\n",
               getpid());

    for (;;) {
        sleep_ms(5000);
        if (battinfo(&bi) == 0 && bi.present) {
            printf("batteryd: %u%% %dmV %s (%d mA)\n",
                   bi.percent, bi.voltage_mv,
                   bi.current_ma > 0 ? "charging" : "draining",
                   bi.current_ma);
        }
    }
    return 0;
}
