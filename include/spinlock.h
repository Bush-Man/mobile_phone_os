#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

#include "irq.h"

/*
 * Minimal ticket-free test-and-set spinlocks built on LDXR/STXR with
 * acquire/release semantics. IRQ-safe variants mask interrupts while
 * held -- that is what all scheduler paths use, since timer ticks may
 * otherwise try to take the same lock on top of their own cpu.
 */

typedef struct {
    volatile uint32_t word;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l)
{
    uint32_t val, res;

    __asm__ volatile(
        "1:\n"
        "   ldaxr   %w0, %2\n"
        "   cbnz    %w0, 1b\n"
        "   mov     %w0, #1\n"
        "   stxr    %w1, %w0, %2\n"
        "   cbnz    %w1, 1b\n"
        : "=&r"(val), "=&r"(res), "=Q"(l->word)
        :
        : "memory");
}

static inline void spin_unlock(spinlock_t *l)
{
    __asm__ volatile("stlr wzr, [%0]" :: "r"(l) : "memory");
}

typedef daif_state irqsave_state;

static inline void spin_lock_irqsave(spinlock_t *l, irqsave_state *s)
{
    *s = irq_local_save();
    spin_lock(l);
}

static inline void spin_unlock_irqrestore(spinlock_t *l, irqsave_state s)
{
    spin_unlock(l);
    irq_local_restore(s);
}

#endif /* SPINLOCK_H */
