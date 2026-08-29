/*
 * user.ld - static user program layout (phase 14).
 *
 * Same boring shape as hello.ld (phase 5): everything starts at
 * USER_CODE_BASE, PT_LOAD segments are mapped by the phase-5
 * loader, text+rodata read/execute, data+bss read/write. Shared by
 * every libc-linked program -- one loader-visible layout means one
 * place to audit.
 *
 * PHDRS is explicit so the loader sees exactly two PT_LOADs: an
 * R+X image segment and an R+W data segment. Without it ld merges
 * everything into one RWX segment.
 */

ENTRY(_start)

PHDRS
{
    text  PT_LOAD FLAGS(5);         /* R + X */
    data  PT_LOAD FLAGS(6);         /* R + W */
}

SECTIONS
{
    . = 0x0000010000000000;

    .text : { *(.text.boot) *(.text*) } :text
    .rodata : { *(.rodata*) } :text

    /*
     * Page gap: separate segments must not share a page, or the
     * kernel's page-granular mapping would give the writable data
     * page execute permission anyway.
     */
    . = ALIGN(0x1000);
    .data : { *(.data*) } :data

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) } :data
    . = ALIGN(16);
    __bss_end = .;

    _end = .;
}
