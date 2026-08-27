/*
 * ipcdemo.ld - static user program layout (phase 8).
 *
 * Same boring shape as hello.ld: everything starts at
 * USER_CODE_BASE, PT_LOAD segments are mapped by the phase-5
 * loader, text+rodata read/execute, data+bss read/write. Kept as a
 * separate file so each user binary owns its build artifact pair.
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