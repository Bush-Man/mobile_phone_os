#ifndef GIC_H
#define GIC_H

#include <stdint.h>

struct platform_info;

/*
 * Low-level interrupt controller backend, selected by gic_init()
 * from the FDT-probed controller version. kernel/irq.c only ever
 * talks to the GIC through these calls so a GICv3 backend can slot
 * in later without touching framework code.
 */
void     gic_init(const struct platform_info *plat);
void     gic_irq_enable(unsigned intid);
void     gic_irq_disable(unsigned intid);
void     gic_irq_set_priority(unsigned intid, unsigned prio);
void     gic_irq_set_trigger_edge(unsigned intid, bool edge);
void     gic_send_sgi(unsigned sgi);

/* returns a full IAR value; id = iar & 0x3ff, 1023 = spurious */
uint32_t gic_ack(void);
void     gic_eoi(uint32_t iar);

#endif /* GIC_H */
