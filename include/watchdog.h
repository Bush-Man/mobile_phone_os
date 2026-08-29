#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

struct platform_info;

/*
 * watchdog.h - the system watchdog (phase 16, plan item 87).
 *
 * QEMU's virt machine has no SP805/SBSA watchdog, so the dev image
 * runs a software watchdog with real teeth: housekeeping kicks it
 * every ~2 ms, and the ARCHITECTED TIMER IRQ -- which keeps firing
 * for as long as interrupts work, independent of the scheduler --
 * checks the deadline. A stale kick is a scheduler-level hang
 * (runqueue wedged, priority inversion, dead housekeeping), and
 * the response is a PSCI system reset after the panic report.
 *
 * HONEST LIMITATION, documented: a hard hang with interrupts
 * masked defeats this design; catching that class needs a real
 * hardware watchdog, which is the board bring-up item (the
 * scaffold below leaves a register() seam for it).
 *
 * The watchdog is OFF until watchdog_arm(): a hung EARLY boot
 * should still be debuggable over serial.
 */

void watchdog_init(const struct platform_info *plat);

/* arm with a stale-kick deadline; disarm cancels                  */
void watchdog_arm(uint32_t timeout_ms);
void watchdog_disarm(void);

/* heartbeat: called from housekeeping every loop                  */
void watchdog_kick(void);

/* deadline check: called from the timer IRQ top half              */
void watchdog_irq_tick(void);

struct watchdog_stats {
    uint64_t kicks;
    bool     armed;
    uint32_t timeout_ms;
    uint32_t misses;            /* stale deadlines seen (pre-reset) */
};

void watchdog_stats_get(struct watchdog_stats *out);

#endif /* WATCHDOG_H */
