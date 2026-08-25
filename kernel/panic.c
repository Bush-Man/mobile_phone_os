/*
 * panic.c - fatal error stop and stack-smashing guard support.
 */

#include <stdint.h>

#include "lib.h"
#include "panic.h"

/* non-zero before any protected function runs; randomized in later phases */
uintptr_t __stack_chk_guard = 0x5f6cfa7d9e62b415ULL;

void panic(const char *msg)
{
    __asm__ volatile("msr daifset, #0xf");
    kprintf("\nPANIC: %s\nSystem halted.\n", msg);

    for (;;)
        __asm__ volatile("wfe");
}

void __stack_chk_fail(void)
{
    panic("stack smashing detected");
}
