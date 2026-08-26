/*
 * hello.ld - static user program layout (phase 5).
 *
 * Linked into the per-process user window (see include/proc.h):
 * everything starts at USER_CODE_BASE. The loader maps PT_LOAD
 * segments from these headers, so the script stays deliberately
 * boring: text+rodata read/execute, data+bss read/write.
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
