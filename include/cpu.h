#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

/*
 * QEMU virt exposes CPUs with linear Aff0 values (0, 1, ...); the
 * board is booted with -smp 2 so two secondaries exist at most.
 * start.S parks anything at or beyond NR_CPUS permanently.
 */
#define NR_CPUS 2u

struct task;

/* per-CPU scheduler state ("per-CPU areas", kept minimal for now) */
struct per_cpu {
    struct task *current;       /* NULL until the cpu enters sched  */
    bool         online;        /* secondary finished bring-up      */
    bool         in_irq;        /* exception context: may not block */
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
