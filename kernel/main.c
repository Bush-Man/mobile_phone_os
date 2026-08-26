/*
 * main.c - phase 3 bring-up sequence.
 */

#include <stdint.h>

#include "el.h"
#include "exceptions.h"
#include "gic.h"
#include "irq.h"
#include "lib.h"
#include "mmio.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"
#include "platform.h"
#include "tasklet.h"
#include "time.h"
#include "uart.h"

extern uint8_t vectors_begin[];
extern uint8_t _start[];

void mem_selftest(void);
void irq_time_selftest(void);

#define BANNER "[OK] mobile_phone_os phase 3"

void kmain(uint64_t boot_el, uint64_t dtb_ptr)
{
    struct platform_info plat;
    struct pmm_stats ps;
    struct kheap_stats ks;
    struct irq_stats is;
    struct tasklet_stats ts;
    unsigned long mark = 0;

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
        kprintf("platform: console UART @ 0x%llx (rx INTID %u)\n",
                (unsigned long long)plat.uart_base, plat.uart_irq);
    if (plat.has_gic)
        kprintf("platform: GICv%d dist 0x%llx cpu 0x%llx (timer INTID %u)\n",
                plat.gic_version,
                (unsigned long long)plat.gicd_base,
                (unsigned long long)plat.gicc_base,
                plat.timer_irq);
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

    gic_init(&plat);
    kprintf("irq: GICv%d online\n", plat.gic_version);

    time_init(&plat);
    kprintf("time: %u Hz system counter, %u Hz tick (INTID %u)\n",
            time_counter_hz(), TIME_HZ, plat.timer_irq);

    uart_rx_irq_init(plat.uart_irq);
    kprintf("uart: interrupt-driven echo armed\n");

    irq_time_selftest();

    irq_stats_get(&is);
    tasklet_stats_get(&ts);
    kprintf("selftest: irq (sgi, chain, timer) ok "
            "(raised %llu handled %llu, tasklets ran %llu)\n",
            (unsigned long long)is.raised,
            (unsigned long long)is.handled,
            (unsigned long long)ts.ran);

    kprintf("%s\n", BANNER);

    for (;;) {
        uint64_t ms;

        tasklet_drain();                    /* bottom halves here     */

        /* milestone demo: one uptime line per second of wall time */
        if ((long)(jiffies_read() - mark) >= TIME_HZ) {
            mark += TIME_HZ;
            ms = time_uptime_ms();
            kprintf("time: %llu.%03llu s uptime\n",
                    (unsigned long long)(ms / 1000u),
                    (unsigned long long)(ms % 1000u));
        }
        __asm__ volatile("wfi");
    }
}
