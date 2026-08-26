/*
 * smp.c - SMP bring-up.
 *
 * QEMU -kernel boots every cpu at the same entry; start.S parks all
 * but CPU0 in a mailbox poll. Release protocol (works across the
 * cached/uncached boundary because secondaries run MMU-off until
 * they activate their own MMU):
 *
 *   1. boot cpu cleans the whole kernel image to DRAM once
 *      (vmm_sync_kernel_to_ram) so page tables/.data are visible;
 *   2. boot cpu fills sec_stacks[cpu], cleans those lines, sets the
 *      cpu's bit in sec_go, dsb + SEV;
 *   3. the woken core switches to its stack and runs
 *      secondary_start(): MMU/caches -> vectors -> GIC cpu
 *      interface -> per-cpu timer -> scheduler idle loop.
 */

#include <stdint.h>

#include "cpu.h"
#include "exceptions.h"
#include "gic.h"
#include "irq.h"
#include "lib.h"
#include "panic.h"
#include "task.h"
#include "time.h"
#include "mm/vmm.h"

extern volatile uint64_t sec_go;
extern uint64_t sec_stacks[8];

static uint8_t secondary_stack_mem[NR_CPUS][16 * 1024]
    __attribute__((aligned(16)));

static void clean_line(uintptr_t va)
{
    __asm__ volatile("dc cvac, %0" :: "r"(va));
}

void smp_init(void)
{
    vmm_sync_kernel_to_ram();           /* tables/.data visible to cold cores */

    for (uint64_t c = 1; c < NR_CPUS; c++) {
        uintptr_t top = (uintptr_t)secondary_stack_mem[c]
                        + sizeof(secondary_stack_mem[c]);

        sec_stacks[c] = top & ~0xfUL;
    }

    /* clean mailbox lines, then release with a global event */
    clean_line((uintptr_t)&sec_stacks[0]);
    clean_line((uintptr_t)&sec_stacks[1]);
    __asm__ volatile("dsb sy");

    sec_go = (1UL << NR_CPUS) - 2UL;    /* every bit except cpu0 */
    clean_line((uintptr_t)&sec_go);
    __asm__ volatile("dsb sy");
    __asm__ volatile("sev");
}

/* ---- running on a secondary from here -------------------------------------- */

static void announce(void)
{
    kprintf("smp: cpu%llu online\n",
            (unsigned long long)cpu_id());
}

void secondary_start(uint64_t cpu)
{
    if (cpu >= NR_CPUS)
        goto park;

    vmm_cpu_activate();
    vectors_init();
    gic_cpu_init();
    time_cpu_init();

    irq_local_unmask();

    cpus[cpu].online = true;
    announce();

    /* adopt this cpu's idle task and enter the shared run queue */
    cpus[cpu].current = &tasks[IDLE_TASK_BASE + cpu];
    tasks[IDLE_TASK_BASE + cpu].state = TASK_RUNNING;
    idle_loop();

park:
    for (;;)
        __asm__ volatile("wfe");
}
