/*
 * main.c - first C code: phase 0 bring-up banner.
 */

#include <stdint.h>

#include "lib.h"
#include "uart.h"

#define BANNER "[OK] mobile_phone_os phase 0"

void kmain(uint64_t el, uint64_t dtb)
{
    uart_init();

    kprintf("\nmobile_phone_os kernel\n");
    kprintf("boot: entered at EL%llu\n", (unsigned long long)el);
    kprintf("boot: DTB pointer = %p\n", (void *)(uintptr_t)dtb);
    kprintf("%s\n", BANNER);

    for (;;)
        __asm__ volatile("wfi");
}
