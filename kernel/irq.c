/*
 * irq.c - interrupt dispatch framework.
 *
 * Handlers are registered by GIC interrupt id. irq_dispatch() is the
 * single entry point from the exception vectors: it drains every
 * pending interrupt (ack -> handler -> eoi loop) before returning so
 * one vector entry can service any number of raised lines. Interrupts
 * never nest; deferred work goes through tasklets instead.
 */

#include <stdint.h>
#include <stdbool.h>

#include "gic.h"
#include "irq.h"
#include "lib.h"
#include "panic.h"

struct irq_entry {
    irq_handler_t fn;
    void          *arg;
    const char    *name;
};

static struct irq_entry table[NR_IRQS];
static struct irq_stats stats;
static bool unhandled_warned;

bool irq_register(unsigned intid, const char *name,
                  irq_handler_t fn, void *arg)
{
    daif_state s;

    if (!fn || intid >= NR_IRQS)
        return false;

    s = irq_local_save();
    if (table[intid].fn) {
        irq_local_restore(s);
        return false;                   /* slot already claimed */
    }
    table[intid].fn   = fn;
    table[intid].arg  = arg;
    table[intid].name = name;
    irq_local_restore(s);
    return true;
}

void irq_enable(unsigned intid)
{
    if (intid < NR_IRQS)
        gic_irq_enable(intid);
}

void irq_disable(unsigned intid)
{
    if (intid < NR_IRQS)
        gic_irq_disable(intid);
}

void irq_set_priority(unsigned intid, unsigned prio)
{
    if (intid < NR_IRQS)
        gic_irq_set_priority(intid, prio);
}

void irq_set_trigger_edge(unsigned intid, bool edge)
{
    if (intid < NR_IRQS)
        gic_irq_set_trigger_edge(intid, edge);
}

void irq_send_sgi(unsigned sgi)
{
    gic_send_sgi(sgi);
}

void irq_stats_get(struct irq_stats *out)
{
    daif_state s = irq_local_save();

    *out = stats;
    irq_local_restore(s);
}

/* ---- exception-vector side ---------------------------------------------- */

static void run_one(uint32_t iar)
{
    unsigned intid = iar & 0x3ffu;
    const struct irq_entry *e;

    stats.raised++;

    e = &table[intid];
    if (e->fn && e->fn(e->arg)) {
        stats.handled++;
        return;
    }

    stats.unhandled++;
    if (!unhandled_warned) {
        unhandled_warned = true;
        kprintf("irq: no handler for INTID %u (%s)\n",
                intid, e->name ? e->name : "anonymous");
    }
}

void irq_dispatch(void)
{
    /*
     * Drain until the controller reports nothing pending. IDs
     * 1020-1023 all mean "nothing to signal" (1023 canonical
     * spurious, 1022 pending-but-filtered); none of them may be
     * written back to EOIR.
     */
    for (;;) {
        uint32_t iar = gic_ack();
        unsigned intid = iar & 0x3ffu;

        if (intid >= 1020u) {
            stats.spurious++;
            break;
        }
        run_one(iar);
        gic_eoi(iar);
    }
}
