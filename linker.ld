/*
 * linker.ld - kernel memory layout for QEMU virt / AArch64.
 * QEMU virt RAM starts at 0x40000000; the ELF is loaded there by -kernel.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text.boot : { KEEP(*(.text.boot)) }

    .text : { *(.text*) }

    .rodata : { *(.rodata*) }

    .data : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    _end = .;
}
