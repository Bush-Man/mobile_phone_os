/*
 * phase16.c - hardening & release polish bring-up (phase 16 entry).
 *
 * The hardening surfaces are already live by the time this runs:
 * the permission registry gates the service transports and the fb0
 * presentation ioctls (syscall.c), the kmsg ring rides every
 * kprintf (lib/printf.c -> kernel/kmsg.c), the ASLR PRNG was seeded
 * in proc_subsys_init, the stack guard was re-randomized in kmain,
 * and the panic path paints + persists. This entry arms the
 * software watchdog and spawns "reltest" (kernel/selftest_release.c),
 * the release battery -- see docs/PHASE_16.md.
 */

#include <stdint.h>
#include <stddef.h>

#include "platform.h"
#include "task.h"
#include "watchdog.h"

void release_selftest_task(void *arg);  /* kernel/selftest_release.c */

void phase16_init(const struct platform_info *plat)
{
    static bool done;

    (void)plat;
    if (done)
        return;
    done = true;

    /*
     * Arm with a generous deadline: housekeeping kicks every ~2 ms,
     * so 10 s of silence means the scheduler is well and truly
     * wedged. The timer IRQ carries the check (drivers/watchdog.c).
     */
    watchdog_arm(10000);

    task_create("reltest", release_selftest_task, NULL, 59);
}
