/*
 * watchdog.c - software watchdog with timer-IRQ teeth (phase 16,
 * plan item 87).
 *
 * Contract (see include/watchdog.h): housekeeping kicks every
 * loop; the timer IRQ -- the last thing still beating when the
 * scheduler wedges -- checks the deadline from irq_tick() and, on
 * a miss, reports through the kmsg ring + console and resets via
 * PSCI. Reset-from-IRQ is deliberate: a wedged scheduler means
 * no task will ever reap the decision.
 */

#include <stdbool.h>
#include <stdint.h>

#include "kmsg.h"
#include "lib.h"
#include "panic.h"
#include "psci.h"
#include "time.h"
#include "uart.h"
#include "watchdog.h"

static bool     armed;
static uint32_t timeout_ms;
static uint64_t last_kick_ms;
static uint64_t kicks;
static uint32_t misses;

void watchdog_init(const struct platform_info *plat)
{
    (void)plat;
    armed = false;
    timeout_ms = 0;
    last_kick_ms = 0;
    kicks = 0;
    misses = 0;
}

void watchdog_arm(uint32_t t_ms)
{
    last_kick_ms = time_uptime_ms();
    timeout_ms = t_ms;
    armed = t_ms != 0;
    kprintf("watchdog: armed (%u ms deadline)\n", t_ms);
}

void watchdog_disarm(void)
{
    armed = false;
    kprintf("watchdog: disarmed\n");
}

void watchdog_kick(void)
{
    kicks++;
    last_kick_ms = time_uptime_ms();
}

void watchdog_irq_tick(void)
{
    uint64_t now;

    if (!armed)
        return;

    now = time_uptime_ms();
    if (now - last_kick_ms < (uint64_t)timeout_ms)
        return;

    /*
     * Stale. Report once (the ring + console are the post-mortem),
     * then reset -- do NOT try to unwind: the whole point is that
     * the scheduler can no longer be trusted.
     */
    if (misses == 0) {
        /*
         * raw mode + best-effort report; psci reset never returns
         */
        uart_panic_mode();
        kprintf("watchdog: STALE KICK (no heartbeat for %u ms)\n",
                (unsigned)(now - last_kick_ms));
        kmsg_dump("/var/kmsg");
    }
    misses++;
    psci_system_reset();
}

void watchdog_stats_get(struct watchdog_stats *out)
{
    out->kicks = kicks;
    out->armed = armed;
    out->timeout_ms = timeout_ms;
    out->misses = misses;
}
