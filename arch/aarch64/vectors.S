/*
 * vectors.S - AArch64 exception vector table for EL1.
 *
 * Layout mandated by ARMv8-A: 16 entries of 128 bytes each
 * (4 kinds x {current EL SP0, current EL SPx, lower EL A64, lower EL A32}).
 * Every stub saves the full trap_frame defined in exceptions.h,
 * calls exceptions_handler(tf, kind), restores state and ERETs.
 *
 * The interrupted SP equals frame base + 280 (nothing else was pushed),
 * so the frame is released with a single ADD at the end.
 */

.section .vectors, "ax"
.align  11                          /* VBAR requires 2 KiB alignment */

.macro STUB kind
    .balign 128
    sub     sp, sp, #280             /* sizeof(struct trap_frame)     */
    stp     x0, x1,  [sp, #0]
    stp     x2, x3,  [sp, #16]
    stp     x4, x5,  [sp, #32]
    stp     x6, x7,  [sp, #48]
    stp     x8, x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]          /* LR                            */
    mrs     x21, esr_el1             /* scratch after x21 was saved   */
    mrs     x22, elr_el1
    mrs     x23, spsr_el1
    add     x24, sp, #280            /* interrupted SP                */
    stp     x21, x22, [sp, #248]     /* ESR, ELR                      */
    stp     x23, x24, [sp, #264]     /* SPSR, SP                      */
    mov     x0, sp                   /* arg0: struct trap_frame *     */
    mov     w1, #\kind               /* arg1: enum exc_kind           */
    bl      exceptions_handler
    ldp     x21, x22, [sp, #248]
    ldp     x23, x24, [sp, #264]
    msr     esr_el1, x21             /* restore system registers      */
    msr     elr_el1, x22
    msr     spsr_el1, x23
    ldp     x0, x1,  [sp, #0]
    ldp     x2, x3,  [sp, #16]
    ldp     x4, x5,  [sp, #32]
    ldp     x6, x7,  [sp, #48]
    ldp     x8, x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #280
    eret
.endm

.global vectors_begin
vectors_begin:

/* --- current EL with SP0 --- */
STUB 0                              /* sync   */
STUB 1                              /* IRQ    */
STUB 2                              /* FIQ    */
STUB 3                              /* SError */

/* --- current EL with SPx --- */
STUB 0
STUB 1
STUB 2
STUB 3

/* --- lower EL, AArch64 --- */
STUB 0
STUB 1
STUB 2
STUB 3

/* --- lower EL, AArch32 --- */
STUB 0
STUB 1
STUB 2
STUB 3

.global vectors_end
vectors_end:
