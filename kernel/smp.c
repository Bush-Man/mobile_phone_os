/*
 * smp.c - SMP bring-up via PSCI_CPU_ON.
 *
 * On QEMU virt the secondaries never execute our _start: they sit in
 * the firmware parking pen described by the DTB /psci node (conduit
 * "hvc", probed by platform.c). Release protocol:
 *
 *   1. boot cpu cleans the whole kernel image to DRAM once
 *      (vmm_sync_kernel_to_ram) so page tables/.data are visible to
 *      a cache-cold core;
 *   2. per-cpu kernel stacks are written and their cache lines
 *      pushed to DRAM via the uncached device window (vmm);
 *   3. PSCI_CPU_ON(mpidr, secondary_entry, cpu_index) powers each
 *      core straight into sec_entry.S, which installs its stack and
 *      calls secondary_start(): MMU/caches -> vectors -> GIC cpu
 *      interface -> per-cpu timer -> shared run queue.
 */

#include <stdint.h>

#include "cpu.h"
#include "exceptions.h"
#include "gic.h"
#include "irq.h"
#include "lib.h"
#include "mmio.h"
#include "panic.h"
#include "platform.h"
#include "smp.h"
#include "task.h"
#include "time.h"
#include "mm/vmm.h"

#define PSCI_RET_SUCCESS 0

uint64_t sec_stacks[NR_CPUS];
const struct platform_info *smp_plat;

static uint8_t secondary_stack_mem[NR_CPUS][16 * 1024]
    __attribute__((aligned(16)));

/* PSCI conduit call; QEMU intercepts hvc at EL1 and emulates it */
static int64_t psci_call(uint64_t fn, uint64_t a0, uint64_t a1,
                         uint64_t a2)
{
    register uint64_t r0 __asm__("x0") = fn;
    register uint64_t r1 __asm__("x1") = a0;
    register uint64_t r2 __asm__("x2") = a1;
    register uint64_t r3 __asm__("x3") = a2;

    if (smp_plat->psci_hvc)
        __asm__ volatile("hvc #0" : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3)
                         : "memory");
    else
        __asm__ volatile("smc #0" : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3)
                         : "memory");
    return (int64_t)r0;
}

void smp_init(void)
{
    vmm_sync_kernel_to_ram();   /* tables/.data visible to cold cores */

    for (uint64_t c = 0; c < NR_CPUS; c++) {
        uintptr_t top = (uintptr_t)secondary_stack_mem[c]
                        + sizeof(secondary_stack_mem[c]);

        sec_stacks[c] = top & ~0xfUL;
    }

    if (!smp_plat || !smp_plat->has_psci)
        panic("smp: no PSCI conduit in device tree");

    for (uint64_t c = 1; c < NR_CPUS; c++) {
        int64_t ret = psci_call(smp_plat->psci_cpu_on_fn,
                                c /* MPIDR Aff0 */, 
                                (uint64_t)(uintptr_t)secondary_entry,
                                c);

        if (ret != PSCI_RET_SUCCESS)
            kprintf("smp: cpu%llu CPU_ON failed (%lld)\n",
                    (unsigned long long)c, (long long)ret);
    }
}

/* ---- running on a secondary from here -------------------------------------- */

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
    kprintf("smp: cpu%llu online\n", (unsigned long long)cpu);

    /* become this cpu's scheduler/idle context */
    sched_run(cpu);

park:
    for (;;)
        __asm__ volatile("wfe");
}
