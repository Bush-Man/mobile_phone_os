/*
 * start.S - first instructions executed after reset.
 * CPU0 installs a boot stack, clears .bss and enters C via
 * kmain(current_EL, dtb_ptr). Every other cpu parks in a mailbox
 * poll loop until kernel/smp.c hands it a stack and releases it
 * (see sec_go/sec_stacks -- written cache-cleaned, polled with an
 * acquire load so the MMU-off view sees them).
 */

.section .text.boot

.global _start
_start:
    msr     daifset, #0xf           /* mask all exceptions until ready */
    /* x0 = current exception level number (1=EL1, 2=EL2, 3=EL3) */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2

    /* secondary cores branch to the parking pen */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, .Lsecondary_park

    /* temporary boot stack */
    ldr     x2, =_stack_top
    mov     sp, x2

    /* zero .bss, 8 bytes at a time */
    ldr     x3, =__bss_start
    ldr     x4, =__bss_end
.Lclear_bss:
    cmp     x3, x4
    b.hs    .Lbss_done
    str     xzr, [x3], #8
    b       .Lclear_bss
.Lbss_done:

    /* x1 = DTB pointer left by firmware (0 on QEMU direct -kernel boot) */
    bl      kmain

    /* kmain never returns; park defensively */
.Lforever:
    wfe
    b       .Lforever

/* ---- secondary parking pen ------------------------------------------------ */

.Lsecondary_park:
    /* x1 = cpu index (MPIDR Aff0) */
    mov     x5, x1

    /* release only happens through the mailboxes below */
.Lpark_poll:
    wfe                             /* low-power wait for the SEV   */
    ldr     x6, =sec_go             /* acquire-load the go bitmask  */
    ldar    x7, [x6]
    lsr     x7, x7, x5
    tbz     x7, #0, .Lpark_poll

    ldr     x6, =sec_stacks
    ldr     x2, [x6, x5, lsl #3]    /* this cpu's kernel stack top  */
    mov     sp, x2

    mov     x0, x5                  /* arg: cpu index               */
    bl      secondary_start         /* C, never returns             */
    b       .Lforever

/* ---- boot-cpu stack -------------------------------------------------------- */

.section .bss.stack
.align  4
.global _stack_bottom
_stack_bottom:
.skip   65536                    /* 64 KiB temporary boot stack */
.global _stack_top
_stack_top:

/* ---- SMP mailboxes ---------------------------------------------------------- */
/* Written by smp.c with dcache cleaned + dsb before the SEV. */

.bss
.align  3
.global sec_go
sec_go:
.skip   8                        /* one bit per cpu              */

.global sec_stacks
sec_stacks:
.skip   8 * 8                    /* stack top per cpu            */
