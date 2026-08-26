/*
 * task.c - tasks, kernel stacks and the task-table API.
 *
 * All tasks live in a static pool with statically allocated kernel
 * stacks -- no dynamic allocation on the task path yet. A new task's
 * context is crafted by hand: sp at the top of its stack, lr pointing
 * at task_first_entry(), so the first cpu_switch_to() "returns" into
 * the trampoline which calls the task body.
 */

#include <stdint.h>
#include <stddef.h>

#include "cpu.h"
#include "irq.h"
#include "lib.h"
#include "panic.h"
#include "spinlock.h"
#include "task.h"
#include "time.h"

struct task tasks[MAX_TASKS];

static uint8_t task_stacks[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

/*
 * One lock guards every state transition in the system (task table,
 * run-queue keys, wait queues, deadlines); rq_seq hands out FIFO
 * tickets for round-robin fairness among equal priorities.
 */
spinlock_t task_state_lock = SPINLOCK_INIT;
static uint64_t rq_seq;

uint64_t task_next_key(void)
{
    return ++rq_seq;
}

/* defined in sched.c */
void schedule(void);

struct task *current_task(void)
{
    return this_cpu()->current;
}

/* ---- creation ------------------------------------------------------------- */

static struct task *alloc_task(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED)
            return &tasks[i];
    return NULL;
}

/*
 * Caller must hold the scheduler lock. Crafts the context the first
 * switch into this task will restore.
 */
static void task_prime(struct task *t, const char *name,
                       void (*fn)(void *), void *arg, unsigned prio)
{
    uintptr_t top = (uintptr_t)task_stacks[t - tasks] + TASK_STACK_SIZE;

    memset(&t->ctx, 0, sizeof(t->ctx));
    t->ctx.sp = top & ~0xfUL;       /* ABI-aligned               */
    t->ctx.lr = (uint64_t)task_first_entry;

    t->state = TASK_READY;
    t->prio  = prio;
    t->rq_key = ++rq_seq;
    t->quantum_left = 0;
    t->wake_at = 0;
    t->wq_next = NULL;
    t->name  = name;
    t->fn    = fn;
    t->arg   = arg;
}

int task_create(const char *name, void (*fn)(void *), void *arg,
                unsigned prio)
{
    daif_state s;
    struct task *t;

    if (!fn)
        return -1;

    spin_lock_irqsave(&task_state_lock, &s);
    t = alloc_task();
    if (!t) {
        spin_unlock_irqrestore(&task_state_lock, s);
        return -1;
    }
    task_prime(t, name, fn, arg, prio);
    spin_unlock_irqrestore(&task_state_lock, s);
    return (int)(t - tasks);
}

/* ---- trampoline + exit ------------------------------------------------------ */

void task_first_entry(void)
{
    struct task *t = current_task();

    if (!t)
        panic("task_first_entry without current");
    t->fn(t->arg);
    task_exit();
}

void task_exit(void)
{
    daif_state s;

    if (cpu_id() >= NR_CPUS)
        panic("task_exit on unknown cpu");

    spin_lock_irqsave(&task_state_lock, &s);
    {
        struct task *t = this_cpu()->current;

        if (!t)
            panic("task_exit without current");
        t->state = TASK_DEAD;
    }
    spin_unlock_irqrestore(&task_state_lock, s);
    schedule();                     /* never returns */
    panic("task_exit resumed a dead task");
}

void task_yield(void)
{
    daif_state s;

    spin_lock_irqsave(&task_state_lock, &s);
    if (this_cpu()->current) {
        this_cpu()->current->state = TASK_READY;
        this_cpu()->current->rq_key = task_next_key();
    }
    spin_unlock_irqrestore(&task_state_lock, s);
    schedule();
}

/* ---- blocking primitives ------------------------------------------------ */

static void block_current(enum task_state state)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;

    /* a task switched in via sched_post_irq legitimately runs inside
     * an irq window (and may migrate between cpus), so the only
     * meaningful precondition for blocking is that current exists */
    if (!pc->current)
        panic("blocking call in irq or pre-scheduler context");

    spin_lock_irqsave(&task_state_lock, &s);
    pc->current->state = state;
    spin_unlock_irqrestore(&task_state_lock, s);
    schedule();
}

void msleep(uint64_t msecs)
{
    daif_state s;

    if (msecs == 0) {
        task_yield();
        return;
    }

    spin_lock_irqsave(&task_state_lock, &s);
    this_cpu()->current->wake_at = jiffies_read() + msecs * TIME_HZ / 1000u;
    spin_unlock_irqrestore(&task_state_lock, s);

    block_current(TASK_SLEEPING);
}

void wait_sleep_when(task_cond_t cond, void *cond_ctx,
                     struct waitqueue *wq)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;

    /* a task switched in via sched_post_irq legitimately runs inside
     * an irq window (and may migrate between cpus), so the only
     * meaningful precondition for blocking is that current exists */
    if (!pc->current)
        panic("blocking call in irq or pre-scheduler context");

    /*
     * Evaluate the predicate and enqueue atomically with respect to
     * any waker: both sides touch the condition under the scheduler
     * lock, so a wake between "check" and "sleep" is impossible.
     */
    spin_lock_irqsave(&task_state_lock, &s);
    if (!cond(cond_ctx)) {
        spin_unlock_irqrestore(&task_state_lock, s);
        return;                         /* already satisfied */
    }
    {
        struct task *t = pc->current;

        t->wq_next = wq->head;
        wq->head = t;
        t->state = TASK_BLOCKED;
    }
    spin_unlock_irqrestore(&task_state_lock, s);
    schedule();
}

void wait_sleep(struct waitqueue *wq)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;

    /* a task switched in via sched_post_irq legitimately runs inside
     * an irq window (and may migrate between cpus), so the only
     * meaningful precondition for blocking is that current exists */
    if (!pc->current)
        panic("blocking call in irq or pre-scheduler context");

    spin_lock_irqsave(&task_state_lock, &s);
    {
        struct task *t = pc->current;

        t->wq_next = wq->head;
        wq->head = t;
        t->state = TASK_BLOCKED;
    }
    spin_unlock_irqrestore(&task_state_lock, s);
    schedule();
}

void wait_wake_all(struct waitqueue *wq)
{
    struct task *cur = current_task();
    bool preempt = false;
    daif_state s;

    spin_lock_irqsave(&task_state_lock, &s);
    while (wq->head) {
        struct task *t = wq->head;

        wq->head = t->wq_next;
        t->wq_next = NULL;
        t->state = TASK_READY;
        t->rq_key = task_next_key();
        if (cur && t->prio < cur->prio)
            preempt = true;         /* waiter outranks this cpu */
    }
    spin_unlock_irqrestore(&task_state_lock, s);

    if (preempt)
        this_cpu()->need_resched = true;
}
