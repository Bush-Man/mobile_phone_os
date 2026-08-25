/*
 * linker.ld - kernel memory layout for QEMU virt / AArch64.
 * QEMU virt RAM starts at 0x40000000; the ELF is loaded there by -kernel.
 */

ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);      /* R+X */
    data PT_LOAD FLAGS(6);      /* R+W */
}

SECTIONS
{
    . = 0x40000000;

    .text.boot : { KEEP(*(.text.boot)) } :text

    .vectors : { KEEP(*(.vectors)) } :text

    .text : { *(.text*) } :text

    .rodata : { *(.rodata*) } :text

    .data : { *(.data*) } :data

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) } :data
    . = ALIGN(16);
    __bss_end = .;

    _end = .;
}
