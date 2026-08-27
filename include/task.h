#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

#include "cpu.h"

enum task_state {
    TASK_UNUSED = 0,            /* free slot                       */
    TASK_READY,                 /* runnable, waiting for a cpu     */
    TASK_RUNNING,               /* owns (one of) the cpus          */
    TASK_SLEEPING,              /* timed sleep, deadline pending   */
    TASK_BLOCKED,               /* parked on a wait queue          */
    TASK_DEAD,                  /* exited                          */
};

#define TASK_NAME_MAX   16
#define TASK_STACK_SIZE (16u * 1024u)
#define MAX_TASKS       8

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
    struct proc *proc;              /* NULL = kernel thread (ph. 5) */

    /*
     * Phase 8 sync layer: while this task intends to sleep on a
     * mutex/semaphore it points at that kmutex/ksem. Written and
     * cleared under the sync core's own lock, which is also how the
     * deadlock detector walks ownership chains without racing.
     * NULL whenever blocked somewhere else (or running).
     */
    void *lock_wait;
};

/* ---- lifecycle ---------------------------------------------------------- */

/*
 * Scheduler model (xv6-style): every cpu owns a scheduler context on
 * a dedicated stack. Tasks never switch between themselves -- they
 * park into the per-cpu scheduler (sched_park), which alone selects
 * and loads the next task. A task's context is therefore only ever
 * written while that task is executing, which is what makes
 * cross-cpu picking safe.
 */
void sched_init(void);          /* clear table, nothing adopted yet   */
int  task_create(const char *name, void (*fn)(void *), void *arg,
                 unsigned prio);
void task_exit(void) __attribute__((noreturn));
void task_yield(void);

/* per-cpu scheduler/idle body: never returns (wfi when queue empty) */
void sched_run(uint64_t cpu) __attribute__((noreturn));

struct task *current_task(void);    /* NULL while on scheduler stack  */

/* ---- scheduler core ------------------------------------------------------ */

void sched_tick(void);          /* timer top half: quantum + wakeups  */
void sched_post_irq(void);      /* exception return preemption point  */

/* park the current task into the per-cpu scheduler (never returns) */
void sched_park(void) __attribute__((noreturn));

/* ---- blocking primitives -------------------------------------------------- */

void msleep(uint64_t msecs);

struct waitqueue {
    struct task *head;
};

typedef bool (*task_cond_t)(void *ctx);

void wait_sleep_when(task_cond_t cond, void *cond_ctx,
                     struct waitqueue *wq);
void wait_wake_all(struct waitqueue *wq);

/* shared table (kernel/task.c, scheduler iterates it) */
extern struct task tasks[MAX_TASKS];
void task_first_entry(void);

/* arch/aarch64/switch.S -- context switch primitive (raw contexts) */
void cpu_switch_to(struct cpu_context *from, struct cpu_context *to);

#endif /* TASK_H */
