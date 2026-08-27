/*
 * evreader.ld - static user program layout for the phase-9 input
 * reader; identical boring shape to hello.ld (base at USER_CODE_BASE,
 * loader maps PT_LOADs, text+rodata RX, data+bss RW).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x0000010000000000;

    .text : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }

    . = ALIGN(16);
    .data : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    _end = .;
}