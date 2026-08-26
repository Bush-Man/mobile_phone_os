#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

/*
 * QEMU virt exposes CPUs with linear MPIDR Aff0 values (0, 1, ...);
 * the board is booted with -smp 2. Secondaries are released out of
 * the firmware PSCI parking pen by kernel/smp.c and identified by
 * the same Aff0 index.
 */
#define NR_CPUS 2u

/*
 * Callee-saved context saved by cpu_switch_to() in switch.S.
 * Offsets are mirrored exactly there -- do not reorder.
 */
struct cpu_context {
    uint64_t x19;               /*   0 */
    uint64_t x20;               /*   8 */
    uint64_t x21;               /*  16 */
    uint64_t x22;               /*  24 */
    uint64_t x23;               /*  32 */
    uint64_t x24;               /*  40 */
    uint64_t x25;               /*  48 */
    uint64_t x26;               /*  56 */
    uint64_t x27;               /*  64 */
    uint64_t x28;               /*  72 */
    uint64_t fp;                /*  80  x29 */
    uint64_t lr;                /*  88  resume pc  */
    uint64_t sp;                /*  96  kernel stack pointer */
};                              /* 112 padded */

struct task;

/* per-cpu scheduler state ("per-CPU areas", kept minimal for now) */
struct per_cpu {
    struct task     *current;   /* NULL while on scheduler stack   */
    struct cpu_context sched_ctx;   /* per-cpu scheduler context    */
    bool         online;        /* secondary finished bring-up      */
    bool         in_irq;        /* exception context bookkeeping    */
    bool         need_resched;
    uint64_t     switches;      /* context switches performed       */
};

extern struct per_cpu cpus[NR_CPUS];

static inline uint64_t cpu_id(void)
{
    uint64_t mpidr;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0xffu;       /* Aff0 distinguishes virt CPUs    */
}

static inline struct per_cpu *this_cpu(void)
{
    return &cpus[cpu_id()];
}

#endif /* CPU_H */
