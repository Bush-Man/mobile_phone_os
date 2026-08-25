/*
 * panic.c - fatal error stop and stack-smashing guard support.
 */

#include <stdint.h>

#include "lib.h"
#include "panic.h"

uintptr_t __stack_chk_guard;

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
