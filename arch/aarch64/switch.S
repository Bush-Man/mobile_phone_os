/*
 * switch.S - AArch64 context switch.
 *
 * Only callee-saved state needs saving: caller-saved registers are
 * dead at a C call boundary, and PSTATE/exception state belongs to
 * whatever context the task resumes in (switches happen either in
 * process context or on the preempted task's own kernel stack).
 *
 * struct cpu_context offsets (task.h) are mirrored below.
 */

.text
.align  2

/*
 * void cpu_switch_to(struct task *prev, struct task *next)
 * x0 = prev, x1 = next. Returns by branching to next->ctx.lr,
 * running on next->ctx.sp with next's x19..x28/fp restored.
 */
.global cpu_switch_to
cpu_switch_to:
    /* save prev's context */
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x2, sp
    str     x2, [x0, #96]

    /* restore next's context; lr last: ret enters the new task */
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldr     x2, [x1, #96]
    mov     sp, x2
    ldp     x29, x30, [x1, #80]
    ret
