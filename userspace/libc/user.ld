/*
 * user.ld - static user program layout (phase 14).
 *
 * Same boring shape as hello.ld (phase 5): everything starts at
 * USER_CODE_BASE, PT_LOAD segments are mapped by the phase-5
 * loader, text+rodata read/execute, data+bss read/write. Shared by
 * every libc-linked program -- one loader-visible layout means one
 * place to audit.
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
