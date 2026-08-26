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

#include "irq.h"
#include "lib.h"
#include "panic.h"
#include "task.h"

struct task tasks[MAX_TASKS];

static uint8_t task_stacks[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

static uint64_t rq_seq;             /* FIFO ticket source            */

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
    daif_state s = irq_local_save();
    struct task *t = alloc_task();

    if (!t || !fn) {
        irq_local_restore(s);
        return -1;
    }
    task_prime(t, name, fn, arg, prio);
    irq_local_restore(s);
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

    s = irq_local_save();
    {
        struct task *t = this_cpu()->current;

        if (!t)
            panic("task_exit without current");
        t->state = TASK_DEAD;
    }
    irq_local_restore(s);
    schedule();                     /* never returns */
    panic("task_exit resumed a dead task");
}

void task_yield(void)
{
    daif_state s = irq_local_save();

    if (this_cpu()->current) {
        this_cpu()->current->state = TASK_READY;
        this_cpu()->current->rq_key = ++rq_seq;
    }
    irq_local_restore(s);
    schedule();
}
