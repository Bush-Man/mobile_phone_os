/*
 * main.c - phase 1 bring-up sequence.
 * Order matters: console first, then a safety net (vectors),
 * then discovery, then memory translation.
 */

#include <stdint.h>

#include "el.h"
#include "exceptions.h"
#include "lib.h"
#include "mmu.h"
#include "panic.h"
#include "platform.h"
#include "uart.h"

extern uint8_t vectors_begin[];
extern uint8_t _start[];
extern uint8_t _end[];

#define BANNER "[OK] mobile_phone_os phase 1"

void kmain(uint64_t boot_el, uint64_t dtb_ptr)
{
    struct platform_info plat;

    (void)dtb_ptr;                  /* firmware-provided DTB lands in x1
                                       on real boards; QEMU path embeds
                                       the blob instead */

    uart_init();
    kprintf("\nmobile_phone_os kernel\n");

    /* 1. exception level */
    el_drop_to_el1();
    kprintf("bringup: running at EL%llu (boot EL%llu)\n",
            (unsigned long long)el_current(),
            (unsigned long long)boot_el);

    /* 2. fault safety net */
    vectors_init();
    kprintf("bringup: vectors installed at %p\n", vectors_begin);

    /* 3. platform discovery */
    platform_self(&plat);
    kprintf("platform: model \"%s\"\n", plat.model);
    kprintf("platform: RAM %llu MiB @ 0x%llx\n",
            (unsigned long long)(plat.ram_size >> 20),
            (unsigned long long)plat.ram_base);
    if (plat.has_uart)
        kprintf("platform: console UART @ 0x%llx\n",
                (unsigned long long)plat.uart_base);
    if (plat.has_boot_args)
        kprintf("platform: bootargs \"%s\"\n", plat.boot_args);

    /* 4. relocation sanity check */
    kprintf("image: linked at %p, ends at %p (%llu KiB)\n",
            _start, _end,
            (unsigned long long)((uintptr_t)(_end - _start) >> 10));
    if ((uintptr_t)_start != 0x40000000UL)
        panic("kernel not loaded at its link address");

    /* 5. address translation + caches */
    mmu_enable(plat.ram_base, plat.ram_size);
    kprintf("bringup: MMU enabled, caches on (SCTLR.M=%d)\n", mmu_active());

    kprintf("%s\n", BANNER);
    for (;;)
        __asm__ volatile("wfi");
}
