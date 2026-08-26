#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Interrupt id ranges (GIC numbering, shared with the GICv2 backend).
 * SGIs 0-15 and PPIs 16-31 are per-CPU (banked); SPIs start at 32.
 */
#define IRQ_SGI_BASE    0u
#define IRQ_PPI_BASE    16u
#define IRQ_SPI_BASE    32u
#define NR_IRQS         288u    /* 32 banked + up to 256 SPIs */

/* architected PPIs (ARMv8-A GIC CPU interface numbering) */
#define IRQ_PPI_HYP_TIMER       26u
#define IRQ_PPI_VIRT_TIMER      27u
#define IRQ_PPI_SEC_PHY_TIMER   29u
#define IRQ_PPI_NS_PHY_TIMER    30u

/* spurious id reported by GICC_IAR when nothing is pending */
#define IRQ_SPURIOUS_ID         1023u

/*
 * Top-half handler: runs in IRQ context with nesting disabled.
 * Return false to flag "not mine" (counted as unhandled); deferred
 * work belongs in a tasklet scheduled from inside the handler.
 */
typedef bool (*irq_handler_t)(void *arg);

struct irq_stats {
    uint64_t raised;            /* IRQ/FIQ exceptions dispatched   */
    uint64_t handled;           /* reached a registered handler    */
    uint64_t unhandled;         /* no handler claimed it           */
    uint64_t spurious;          /* INTID 1023 reads                */
};

/* ---- local CPU interrupt masking (DAIF.I) ----------------------------- */

static inline void irq_local_mask(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static inline void irq_local_unmask(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

typedef unsigned long daif_state;

static inline daif_state irq_local_save(void)
{
    daif_state s;

    __asm__ volatile("mrs %0, daif" : "=r"(s));
    irq_local_mask();
    return s;
}

static inline void irq_local_restore(daif_state s)
{
    __asm__ volatile("msr daif, %0" :: "r"(s) : "memory");
}

/* ---- framework API ------------------------------------------------------ */

bool irq_register(unsigned intid, const char *name,
                  irq_handler_t fn, void *arg);
void irq_enable(unsigned intid);
void irq_disable(unsigned intid);
void irq_set_priority(unsigned intid, unsigned prio);   /* 0 = highest */
void irq_set_trigger_edge(unsigned intid, bool edge);
void irq_send_sgi(unsigned sgi);                        /* to this CPU */
void irq_stats_get(struct irq_stats *out);

/* exception-vector entry point: drains all pending interrupts */
void irq_dispatch(void);

#endif /* IRQ_H */
