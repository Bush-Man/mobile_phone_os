/*
 * selftest_irq.c - phase 3 interrupt & timer verification.
 *
 * Proves the full vertical slice in QEMU before the demo runs:
 *   1. a self-targeted GIC SGI (the kernel's "software interrupt",
 *      no SMC/HVC involved) traverses distributor -> CPU interface
 *      -> vectors -> dispatch -> top half;
 *   2. a top half can defer to a tasklet that the main loop drains
 *      (top/bottom half split);
 *   3. the generic timer really ticks jiffies;
 *   4. monotonic time moves forward.
 * Any failure panics instead of limping on.
 */

#include <stdint.h>
#include <stdbool.h>

#include "irq.h"
#include "lib.h"
#include "panic.h"
#include "tasklet.h"
#include "time.h"

#define SELFTEST_SGI_DIRECT 0
#define SELFTEST_SGI_CHAIN  1

/* bounded waits so a dead system panics instead of hanging forever */
#define SPIN_LIMIT 400000000ull

static void spin_until(const volatile bool *cond)
{
    for (volatile unsigned long i = 0; i < SPIN_LIMIT; i++)
        if (*cond)
            return;
    panic("selftest: condition never became true");
}

static void spin_jiffies(unsigned long from, unsigned delta)
{
    for (volatile unsigned long i = 0; i < SPIN_LIMIT; i++)
        if ((long)(jiffies_read() - from) >= (long)delta)
            return;
    panic("selftest: timer stopped ticking");
}

static volatile bool direct_hit;

static bool sgi_direct_top(void *arg)
{
    (void)arg;
    direct_hit = true;
    return true;
}

static volatile bool chain_done;

static void chain_bottom(void *arg)
{
    (void)arg;
    chain_done = true;
}

static bool sgi_chain_top(void *arg)
{
    (void)arg;
    tasklet_schedule(chain_bottom, NULL);   /* defer to process ctx */
    return true;
}

void irq_time_selftest(void)
{
    unsigned long j0;
    uint64_t ns0, ns1;

    /* interrupts are still masked from _start; from here on they run */
    if (!irq_register(IRQ_SGI_BASE + SELFTEST_SGI_DIRECT,
                      "selftest-sgi", sgi_direct_top, NULL) ||
        !irq_register(IRQ_SGI_BASE + SELFTEST_SGI_CHAIN,
                      "selftest-chain", sgi_chain_top, NULL))
        panic("selftest: sgi registration failed");
    irq_enable(IRQ_SGI_BASE + SELFTEST_SGI_DIRECT);
    irq_enable(IRQ_SGI_BASE + SELFTEST_SGI_CHAIN);
    irq_local_unmask();

    /* 1. software interrupt round trip through the whole GIC path */
    irq_send_sgi(SELFTEST_SGI_DIRECT);
    spin_until(&direct_hit);

    /* 2. top half defers; this context drains the bottom half */
    irq_send_sgi(SELFTEST_SGI_CHAIN);
    while (!chain_done)
        tasklet_drain();
    tasklet_drain();

    /* 3. periodic ticks arrive and are counted */
    j0 = jiffies_read();
    spin_jiffies(j0, 3);

    /* 4. monotonic clock strictly increases */
    ns0 = time_uptime_ns();
    for (volatile unsigned long i = 0; i < 1000000ull; i++)
        ;
    ns1 = time_uptime_ns();
    if (ns1 <= ns0)
        panic("selftest: monotonic clock frozen");
}
