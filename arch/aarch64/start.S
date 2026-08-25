/*
 * start.S - first instructions executed after reset.
 * Park secondary cores, install a boot stack, clear .bss,
 * then enter C via kmain(current_EL, dtb_ptr).
 */

.section .text.boot

.global _start
_start:
    msr     daifset, #0xf           /* mask all exceptions until ready */
    /* x0 = current exception level number (1=EL1, 2=EL2, 3=EL3) */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2

    /* park every core except CPU0 (affinity level 0) */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, .Lpark

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

.Lpark:
    wfe
    b       .Lpark

.section .bss.stack
.align  4
.global _stack_bottom
_stack_bottom:
.skip   65536                    /* 64 KiB temporary boot stack */
.global _stack_top
_stack_top:
