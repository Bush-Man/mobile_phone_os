/*
 * el.S - exception level transition trampolines.
 * Each writes SPSR/ELR for an EL1h entry with DAIF masked, then ERETs.
 */

.text
.align  2

/* void el_enter_el1_from_el2(uint64_t target_pc) */
.global el_enter_el1_from_el2
el_enter_el1_from_el2:
    mov     x1, #0x3c5              /* SPSR: M=EL1h, DAIF all masked */
    msr     spsr_el2, x1
    msr     elr_el2, x0
    eret

/* void el_enter_el1_from_el3(uint64_t target_pc) */
.global el_enter_el1_from_el3
el_enter_el1_from_el3:
    mov     x1, #0x3c5              /* SPSR: M=EL1h, DAIF all masked */
    msr     spsr_el3, x1
    msr     elr_el3, x0
    eret
