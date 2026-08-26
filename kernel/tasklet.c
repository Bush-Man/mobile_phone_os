/*
 * tasklet.c - fixed-capacity deferred work queue.
 *
 * Single consumer (the main loop) plus arbitrary producers (IRQ top
 * halves and process context), so enqueue/dequeue sections are guarded
 * by masking IRQs rather than spinlocks. Work functions themselves run
 * with interrupts ENABLED -- they may schedule more tasklets and take
 * their time; the queue only needs to stay consistent.
 */

#include <stdint.h>

#include "irq.h"
#include "lib.h"
#include "tasklet.h"

#define TASKLET_QUEUE 256

struct work_item {
    void (*fn)(void *arg);
    void  *arg;
};

static struct work_item ring[TASKLET_QUEUE];
static unsigned head;                   /* next to run              */
static unsigned tail;                   /* next free slot           */
static bool overflow_reported;
static struct tasklet_stats stats;

void tasklet_schedule(void (*fn)(void *arg), void *arg)
{
    daif_state s;
    unsigned depth;

    if (!fn)
        return;

    s = irq_local_save();
    {
        unsigned next = (tail + 1u) % TASKLET_QUEUE;

        if (next == head) {
            stats.dropped++;
        } else {
            ring[tail].fn  = fn;
            ring[tail].arg = arg;
            tail = next;
            stats.queued++;

            depth = (tail - head) % TASKLET_QUEUE;
            if (depth > stats.peak_depth)
                stats.peak_depth = depth;
        }
    }
    irq_local_restore(s);

    if (!overflow_reported && stats.dropped == 1) {
        overflow_reported = true;
        kprintf("tasklet: queue overflow, work dropped\n");
    }
}

static bool pop(struct work_item *out)
{
    daif_state s = irq_local_save();
    bool got = false;

    if (head != tail) {
        *out = ring[head];
        head = (head + 1u) % TASKLET_QUEUE;
        got = true;
    }
    irq_local_restore(s);
    return got;
}

void tasklet_drain(void)
{
    struct work_item w;
    daif_state s = irq_local_save();

    /*
     * Leave the caller's masking state exactly as found: work runs
     * preemptible only if the caller itself allowed interrupts.
     */
    while (pop(&w)) {
        stats.ran++;
        irq_local_restore(s);
        w.fn(w.arg);
        s = irq_local_save();
    }
    irq_local_restore(s);
}

void tasklet_stats_get(struct tasklet_stats *out)
{
    daif_state s = irq_local_save();

    *out = stats;
    irq_local_restore(s);
}
