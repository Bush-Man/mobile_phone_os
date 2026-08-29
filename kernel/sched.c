/*
 * sched.c - priority round-robin scheduler with per-cpu scheduler
 * contexts (xv6-style).
 *
 * Every cpu runs sched_run() on a dedicated scheduler stack. Tasks
 * never switch between themselves: parking a task saves its context
 * into its own struct and jumps to the scheduler context, which then
 * picks the next READY task under the shared state lock and loads
 * it. Because a task's context is only ever written while that task
 * is actually executing, another cpu can safely pick it the moment
 * it is marked READY.
 *
 * One lock guards all state transitions. The per-cpu idle role is
 * played by the scheduler loop itself: when nothing is READY it
 * drops to WFI until the next interrupt (timer tick at worst).
 */

#include <stdint.h>
#include <stddef.h>

#include "cpu.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "panic.h"
#include "pm.h"
#include "proc.h"
#include "spinlock.h"
#include "task.h"
#include "time.h"

struct per_cpu cpus[NR_CPUS];

/* dedicated stacks for the per-cpu scheduler contexts */
static uint8_t sched_stacks[NR_CPUS][4 * 1024]
    __attribute__((aligned(16)));

#define SCHED_QUANTUM 5         /* ticks (50 ms) before round-robin */

/* shared state plumbing defined in kernel/task.c */
extern spinlock_t task_state_lock;
uint64_t task_next_key(void);

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

void sched_init(void)
{
    for (uint64_t c = 0; c < NR_CPUS; c++) {
        cpus[c].sched_ctx.sp =
            (uint64_t)(uintptr_t)sched_stacks[c] +
            sizeof(sched_stacks[c]);
        cpus[c].sched_ctx.lr = 0;
        cpus[c].current = NULL;
    }
}

/*
 * Per-cpu scheduler/idle body. Runs on its own stack; every task
 * parks here via sched_park() and is dispatched out from here.
 */
void sched_run(uint64_t cpu)
{
    struct per_cpu *pc = &cpus[cpu];

    for (;;) {
        daif_state s;
        struct task *next;

        spin_lock_irqsave(&task_state_lock, &s);
        next = pick_next();
        if (next) {
            next->state = TASK_RUNNING;
            next->quantum_left = SCHED_QUANTUM;
            pc->current = next;
        }
        spin_unlock_irqrestore(&task_state_lock, s);

        if (!next) {
            /*
             * Phase 10: idle goes through the PM governor now --
             * WFI stays the implemented depth (GIC-armed interrupt
             * paths are the wake sources by construction), the
             * governor owns the accounting and the extension point
             * for deeper states on real boards.
             */
            pm_cpu_idle(cpu);
            continue;
        }

        /*
         * Phase 5: install the incoming task's address space before
         * it runs. Always written (never compared): one TTBR0+isb
         * per dispatch is negligible, and "always correct" beats a
         * cached-decision bug. Kernel threads pass proc == NULL and
         * get the shared kernel root with reserved ASID 0.
         */
        proc_address_space_switch(next->proc);

        cpu_switch_to(&pc->sched_ctx, &next->ctx);
        /*
         * Back: that task parked itself again (task_exit, blocking,
         * or preemption). From this instant its context is quiescent
         * -- phase 14 marks the handshake thread reclaim relies on.
         */
        pc->current->parked = true;
    }
}

/* ---- timer-tick side ------------------------------------------------------- */

void sched_tick(void)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;

    spin_lock_irqsave(&task_state_lock, &s);

    /* wake expired sleepers */
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];

        if (t->state == TASK_SLEEPING &&
            (long)(jiffies_read() - t->wake_at) >= 0) {
            t->state = TASK_READY;
            t->rq_key = task_next_key();
        }
    }

    /* quantum expiry -> preempt on the way out of this interrupt */
    if (pc->current && pc->current->state == TASK_RUNNING) {
        if (pc->current->quantum_left > 0)
            pc->current->quantum_left--;
        if (pc->current->quantum_left == 0)
            pc->need_resched = true;
    }

    spin_unlock_irqrestore(&task_state_lock, s);
}

void sched_post_irq(void)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;

    /*
     * Preemption point on the way out of an interrupt: everything
     * is already EOIed, so parking the interrupted task here cannot
     * strand controller state across the switch.
     */
    if (!pc->need_resched || !pc->current)
        return;
    pc->need_resched = false;

    spin_lock_irqsave(&task_state_lock, &s);
    /*
     * Phase 14: a task can have been marked DEAD cross-cpu while it
     * was running (process kill sweeping its threads). Re-queue only
     * what is still alive; a DEAD current just parks for good.
     */
    if (pc->current->state == TASK_RUNNING) {
        pc->current->state = TASK_READY;
        pc->current->rq_key = task_next_key();
    }
    spin_unlock_irqrestore(&task_state_lock, s);

    sched_park();                   /* through the scheduler context */
}
