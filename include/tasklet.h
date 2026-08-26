#ifndef TASKLET_H
#define TASKLET_H

#include <stdint.h>

/*
 * Deferred work queue ("tasklet"): the bottom-half mechanism for
 * phase 3. Top-half handlers schedule a function pointer; it runs
 * later from process context in tasklet_drain(), keeping everything
 * slow (console output, allocations) out of IRQ context. Software
 * notifications use GIC SGIs -- there is no SMC/HVC/trap-based SWI
 * path anywhere in this kernel.
 *
 * Fixed-capacity ring, no dynamic allocation: safe to call from
 * interrupt context.
 */

struct tasklet_stats {
    uint64_t queued;
    uint64_t ran;
    uint64_t dropped;           /* overflowed while queue full */
    uint64_t peak_depth;
};

void tasklet_schedule(void (*fn)(void *arg), void *arg);
void tasklet_drain(void);               /* run all pending work */
void tasklet_stats_get(struct tasklet_stats *out);

#endif /* TASKLET_H */
