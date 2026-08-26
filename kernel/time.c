/*
 * time.c - ARM generic timer driver + timekeeping.
 *
 * Uses the VIRTUAL timer view (CNTV_*), which is always accessible at
 * EL1 without a hypervisor; its maintenance PPI (INTID 27, confirmed
 * against the FDT /timer node by the platform probe) is edge
 * triggered and raised to the highest priority so ticks are never
 * delayed behind device interrupts.
 *
 * The compare is programmed as an absolute one-shot and re-armed from
 * the handler, giving tickless-capable semantics from day one while
 * still delivering TIME_HZ periodic ticks for jiffies.
 */

#include <stdint.h>
#include <stddef.h>

#include "irq.h"
#include "panic.h"
#include "platform.h"
#include "time.h"

static uint32_t counter_hz;             /* CNTFRQ_EL0                   */
static uint64_t period_ticks;           /* counter ticks per jiffy      */
static volatile unsigned long jiffies;
static uint64_t wall_epoch_ns;          /* wall = epoch + monotonic     */

/* ---- counter/timer register access -------------------------------------- */

static inline uint64_t counter_read(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline void compare_write(uint64_t cval)
{
    __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(cval) : "memory");
}

static inline void ctl_write(uint64_t enable)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(enable) : "memory");
}

static inline uint64_t compare_readback(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(v));
    return v;
}

uint32_t time_counter_hz(void)
{
    return counter_hz;
}

unsigned long jiffies_read(void)
{
    return jiffies;
}

/* ---- conversions --------------------------------------------------------- */

/* t in counter ticks -> ns, split into secs+remainder to stay in u64 */
static uint64_t ticks_to_ns(uint64_t t)
{
    return (t / counter_hz) * 1000000000ull +
           (t % counter_hz) * 1000000000ull / counter_hz;
}

uint64_t time_uptime_ns(void)
{
    return ticks_to_ns(counter_read());
}

uint64_t time_uptime_ms(void)
{
    return counter_read() * 1000ull / counter_hz;
}

void time_set_wallclock(uint64_t epoch_ns)
{
    wall_epoch_ns = epoch_ns - time_uptime_ns();
}

uint64_t time_wallclock_ns(void)
{
    return wall_epoch_ns + time_uptime_ns();
}

/* ---- timer programming ----------------------------------------------------- */

void timer_arm_oneshot_ns(uint64_t ns)
{
    uint64_t delta = ns * counter_hz / 1000000000ull;

    if (!delta)
        delta = 1;
    compare_write(counter_read() + delta);
    ctl_write(1);                       /* ENABLE, IMASK clear */
}

void time_restart_periodic(void)
{
    compare_write(counter_read() + period_ticks);
    ctl_write(1);
}

/* top half: runs on every virtual-timer PPI */
static bool timer_tick(void *arg)
{
    (void)arg;

    jiffies++;
    /* absolute re-arm: += period instead of now+period => zero drift */
    compare_write(compare_readback() + period_ticks);
    return true;
}

void time_init(const struct platform_info *plat)
{
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(counter_hz));

    if (counter_hz < TIME_HZ)
        panic("time: broken generic timer frequency");

    period_ticks = counter_hz / TIME_HZ;

    if (!irq_register(plat->timer_irq, "arch-timer", timer_tick, NULL))
        panic("time: timer intid already claimed");
    irq_set_priority(plat->timer_irq, 0x00);        /* above devices */
    irq_set_trigger_edge(plat->timer_irq, true);
    irq_enable(plat->timer_irq);

    /* first tick; handler takes over re-arming from here */
    compare_write(counter_read() + period_ticks);
    ctl_write(1);
}
