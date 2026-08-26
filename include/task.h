#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

#include "cpu.h"

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

enum task_state {
    TASK_UNUSED = 0,            /* free slot                       */
    TASK_READY,                 /* runnable, waiting for a cpu     */
    TASK_RUNNING,               /* owns (one of) the cpus          */
    TASK_SLEEPING,              /* timed sleep, deadline pending   */
    TASK_BLOCKED,               /* parked on a wait queue          */
    TASK_DEAD,                  /* exited, slot not reused yet     */
};

#define TASK_NAME_MAX   16
#define TASK_STACK_SIZE (16u * 1024u)
#define MAX_TASKS       8
#define TASK_IDLE_PRIO  0xffffu    /* loses to every real task      */

/* slots 0..NR_CPUS-1 are reserved for the per-cpu idle tasks */
#define IDLE_TASK_BASE  0

struct task {
    struct cpu_context ctx;         /* must stay at offset 0        */
    volatile enum task_state state;
    unsigned prio;                  /* lower value = higher prio    */
    uint64_t rq_key;                /* FIFO order among equal prio  */
    uint64_t quantum_left;          /* ticks until preempt check    */
    uint64_t wake_at;               /* jiffies deadline (sleeping)  */
    struct task *wq_next;           /* wait-queue linkage           */
    const char *name;
    void (*fn)(void *arg);
    void *arg;
};

/* ---- lifecycle ---------------------------------------------------------- */

void sched_init(void);      /* build idle tasks, cpu0 adopts idle */
int  task_create(const char *name, void (*fn)(void *), void *arg,
                 unsigned prio);
void task_exit(void) __attribute__((noreturn));
void task_yield(void);
void idle_loop(void) __attribute__((noreturn));

struct task *current_task(void);    /* NULL before a cpu enters sched */

/* ---- scheduler core ------------------------------------------------------ */

void schedule(void);
void sched_tick(void);          /* timer top half: quantum + wakeups  */
void sched_post_irq(void);      /* exception return preemption point  */

/* ---- blocking primitives -------------------------------------------------- */

void msleep(uint64_t msecs);

struct waitqueue {
    struct task *head;
};

void wait_sleep(struct waitqueue *wq);
void wait_wake_all(struct waitqueue *wq);

/* shared table (kernel/task.c, scheduler iterates it) */
extern struct task tasks[MAX_TASKS];
void task_first_entry(void);

/* arch/aarch64/switch.S -- context switch primitive */
void cpu_switch_to(struct task *prev, struct task *next);

#endif /* TASK_H */
