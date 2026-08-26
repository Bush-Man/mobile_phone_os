#ifndef TIME_H
#define TIME_H

#include <stdint.h>

struct platform_info;

/*
 * Periodic tick rate driving jiffies. The hardware programming is
 * one-shot compare re-armed from the handler (no auto-reload mode on
 * the architected timer), so switching later phases to true tickless
 * operation only changes who calls timer_arm_oneshot_ns().
 */
#define TIME_HZ 100u

void     time_init(const struct platform_info *plat);

uint32_t time_counter_hz(void);         /* raw system counter frequency */
unsigned long jiffies_read(void);       /* ticks since boot, wraps      */

/* monotonic since boot */
uint64_t time_uptime_ns(void);
uint64_t time_uptime_ms(void);

/* wall clock = adjustable epoch offset + monotonic (starts at 0) */
void     time_set_wallclock(uint64_t epoch_ns);
uint64_t time_wallclock_ns(void);

/* tickless-ready primitive: fire the timer once after ns nanoseconds
 * (replaces the periodic re-arm until time_restart_periodic())      */
void     timer_arm_oneshot_ns(uint64_t ns);
void     time_restart_periodic(void);

#endif /* TIME_H */
