/*
 * main.c - phase 2 bring-up sequence.
 */

#include <stdint.h>

#include "el.h"
#include "exceptions.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"
#include "platform.h"
#include "uart.h"

extern uint8_t vectors_begin[];
extern uint8_t _start[];

void mem_selftest(void);

#define BANNER "[OK] mobile_phone_os phase 2"

void kmain(uint64_t boot_el, uint64_t dtb_ptr)
{
    struct platform_info plat;
    struct pmm_stats ps;
    struct kheap_stats ks;

    (void)dtb_ptr;

    uart_init();
    kprintf("\nmobile_phone_os kernel\n");

    el_drop_to_el1();
    kprintf("bringup: running at EL%llu (boot EL%llu)\n",
            (unsigned long long)el_current(),
            (unsigned long long)boot_el);

    vectors_init();
    kprintf("bringup: vectors installed at %p\n", vectors_begin);

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

    if ((uintptr_t)_start != 0x40000000UL)
        panic("kernel not loaded at its link address");

    vmm_init(&plat);
    kprintf("bringup: memory management up (TTBR0/TTBR1 + caches on)\n");

    mem_selftest();

    pmm_stats_get(&ps);
    kheap_stats_get(&ks);
    kprintf("mm: %llu/%llu frames free, heap %llu/%llu allocs\n",
            (unsigned long long)ps.free_frames,
            (unsigned long long)ps.total_frames,
            (unsigned long long)ks.allocs,
            (unsigned long long)ks.frees);

    kprintf("%s\n", BANNER);
    for (;;)
        __asm__ volatile("wfi");
}
