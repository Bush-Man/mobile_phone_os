/*
 * user_entry.S - kernel -> EL0 transitions (phase 5).
 *
 * proc_enter_user(): x0 = struct trap_frame * describing the very
 * first user context of a process (entry PC, user SP, argc/argv in
 * x0..x2, SPSR = EL0t). Installs the frame as the current kernel
 * stack top and erets into user mode. Never returns.
 *
 * proc_tf_eret(): shared "restore a full trap frame and eret"
 * tail. Unlike the vector epilogue it ALWAYS writes SP_EL0: every
 * caller here returns to EL0 by construction. SP_EL1 is left just
 * above the consumed frame so the next exception from this task
 * rebuilds its frame at the same spot.
 *
 * Frame layout mirrors struct trap_frame / vectors.S exactly.
 */

.text
.align  2

.global proc_enter_user
proc_enter_user:
    mov     sp, x0
    /* fall through */

.global proc_tf_eret
proc_tf_eret:
    ldp     x21, x22, [sp, #248]    /* esr, elr                        */
    ldp     x23, x24, [sp, #264]    /* spsr, interrupted sp            */
    msr     elr_el1, x22
    msr     spsr_el1, x23
    msr     sp_el0, x24             /* user stack pointer              */
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
