/*
 * sched.c - priority round-robin scheduler.
 *
 * One shared run-queue: pick_next scans the (small) static task table
 * for the READY task with the best (lowest) priority, breaking ties by
 * arrival order -- that is round-robin among equals and, because every
 * cpu pulls from the same table, it doubles as basic load balancing
 * with zero migration logic.
 *
 * All scheduler state is guarded by one spinlock taken with IRQs
 * masked; the timer top half only sets need_resched and the actual
 * switch happens on the exception return path after EOIs are done, so
 * no interrupt stays active across a context switch.
 */

#include <stdint.h>
#include <stddef.h>

#include "irq.h"
#include "lib.h"
#include "panic.h"
#include "spinlock.h"
#include "task.h"
#include "time.h"

struct per_cpu cpus[NR_CPUS];

static spinlock_t sched_lock = SPINLOCK_INIT;
static uint64_t rq_seq;

#define SCHED_QUANTUM 5         /* ticks (50 ms) before round-robin */

static inline bool better(const struct task *a, const struct task *b)
{
    if (a->prio != b->prio)
        return a->prio < b->prio;
    return a->rq_key < b->rq_key;
}

/* lock held */
static struct task *pick_next(void)
{
    struct task *best = NULL;

    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];

        if (t->state != TASK_READY)
            continue;
        if (!best || better(t, best))
            best = t;
    }
    return best;
}

void schedule(void)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;
    struct task *prev, *next;

    spin_lock_irqsave(&sched_lock, &s);

    prev = pc->current;
    if (!prev)
        panic("schedule without a current task");
    next = pick_next();

    if (!next) {
        /* nothing runnable: stay where we are (idle keeps polling) */
        spin_unlock_irqrestore(&sched_lock, s);
        return;
    }

    if (prev && prev->state == TASK_RUNNING)
        prev->state = TASK_READY;   /* still runnable, just yielding */

    next->state = TASK_RUNNING;
    next->quantum_left = SCHED_QUANTUM;
    pc->current = next;
    pc->need_resched = false;
    pc->switches++;

    spin_unlock_irqrestore(&sched_lock, s);

    cpu_switch_to(prev, next);
}

/* ---- timer-tick side ------------------------------------------------------- */

void sched_tick(void)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;

    spin_lock_irqsave(&sched_lock, &s);

    /* wake expired sleepers */
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];

        if (t->state == TASK_SLEEPING &&
            (long)(jiffies_read() - t->wake_at) >= 0) {
            t->state = TASK_READY;
            t->rq_key = ++rq_seq;
        }
    }

    /* quantum expiry -> preempt on the way out of this interrupt */
    if (pc->current && pc->current->state == TASK_RUNNING) {
        if (pc->current->quantum_left > 0)
            pc->current->quantum_left--;
        if (pc->current->quantum_left == 0)
            pc->need_resched = true;
    }

    spin_unlock_irqrestore(&sched_lock, s);
}

void sched_post_irq(void)
{
    struct per_cpu *pc = this_cpu();

    if (pc->need_resched && pc->current) {
        pc->need_resched = false;
        schedule();
    }
}

/* ---- init --------------------------------------------------------------------- */

/*
 * Idle tasks occupy fixed slots 0..NR_CPUS-1. They are never
 * "created" with a crafted context: each cpu adopts its idle task
 * directly (boot cpu in kmain, secondaries in secondary_start), so
 * the first switch away simply snapshots the live frame.
 */
void sched_init(void)
{
    for (uint64_t c = 0; c < NR_CPUS; c++) {
        struct task *t = &tasks[IDLE_TASK_BASE + c];

        memset(t, 0, sizeof(*t));
        t->state  = TASK_READY;
        t->prio   = TASK_IDLE_PRIO;
        t->rq_key = ++rq_seq;
        t->name   = (c == 0) ? "idle0" : "idle1";
    }

    /* boot cpu adopts its idle task right away */
    cpus[0].current = &tasks[IDLE_TASK_BASE];
    tasks[IDLE_TASK_BASE].state = TASK_RUNNING;
}

/* ---- idle -------------------------------------------------------------------- */

/*
 * The per-cpu idle "thread": runs as a real task at the lowest
 * priority so the shared run queue naturally drains to it. WFI sleeps
 * until the next interrupt (timer tick or device line).
 */
void idle_loop(void)
{
    for (;;) {
        schedule();
        __asm__ volatile("wfi");
    }
}
