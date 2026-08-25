#ifndef EL_H
#define EL_H

#include <stdint.h>

static inline uint64_t el_current(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el >> 2;             /* CurrentEL register holds EL << 2 */
}

/*
 * Drop from EL3 or EL2 down to EL1 (no-op when already at EL1).
 * Continues execution right after the call inside el_drop_to_el1().
 */
void el_drop_to_el1(void);

/* assembly helpers in el.S */
void el_enter_el1_from_el2(uint64_t target_pc);
void el_enter_el1_from_el3(uint64_t target_pc);

#endif /* EL_H */
