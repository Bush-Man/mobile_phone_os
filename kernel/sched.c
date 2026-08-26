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
#include "mmio.h"
#include "panic.h"
#include "spinlock.h"
#include "task.h"
#include "time.h"

struct per_cpu cpus[NR_CPUS];

#define SCHED_QUANTUM 5         /* ticks (50 ms) before round-robin */

/* shared state plumbing from kernel/task.c */
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

void schedule(void)
{
    struct per_cpu *pc = this_cpu();
    daif_state s;
    struct task *prev, *next;

    /*
     * The lock spans the WHOLE switch: prev stays unpickable until
     * its callee-saved context is fully written, and the incoming
     * task resumes exactly here (inside its own old schedule()
     * frame) to release the same lock. Unlocking before the save
     * would let another cpu pick up a half-saved context.
     */
    spin_lock_irqsave(&task_state_lock, &s);

    prev = pc->current;
    if (!prev)
        panic("schedule without a current task");
    next = pick_next();

    if (!next) {
        /* nothing runnable: stay where we are (idle keeps polling) */
        spin_unlock_irqrestore(&task_state_lock, s);
        return;
    }

    if (next != prev) {
        if (prev->state == TASK_RUNNING)
            prev->state = TASK_READY;

        next->state = TASK_RUNNING;
        pc->current = next;
        pc->switches++;
    }
    next->quantum_left = SCHED_QUANTUM;
    pc->need_resched = false;

    cpu_switch_to(prev, next);

    /* resumed as `next` (or fell through switching to ourselves) */
    spin_unlock_irqrestore(&task_state_lock, s);
}

/* ---- timer-tick side ------------------------------------------------------- */

static bool temp_preempt_off = true;

void sched_tick(void)
{
    if (temp_preempt_off) return;
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
    if (temp_preempt_off) return;
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
        t->rq_key = task_next_key();
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
    uint64_t last_counter = time_counter_value();
    unsigned long last_jiffies = jiffies_read();
    bool dumped = false;

    for (;;) {
        schedule();

        /* TEMP: detect "timer died" state and dump controller regs */
        {
            uint64_t now = time_counter_value();
            unsigned long j = jiffies_read();

            if (!dumped && now - last_counter > time_counter_hz() / 5 &&
                j == last_jiffies) {
                dumped = true;
                {
                    uint64_t ctl;
                    __asm__ volatile("mrs %0, cntv_ctl_el0"
                                     : "=r"(ctl));
                    kprintf("[idle%llu stall: j=%lu ctl=%llx ist=%d "
                            "isen0=%08x ispend0=%08x rpr=%08x "
                            "hppir=%08x]\n",
                            (unsigned long long)cpu_id(), j,
                            (unsigned long long)ctl,
                            (int)((ctl >> 2) & 1),
                            mmio_read32(0x08000000u + 0x100u),
                            mmio_read32(0x08000000u + 0x200u),
                            mmio_read32(0x08010000u + 0x014u),
                            mmio_read32(0x08010000u + 0x018u));
                }
            }
            if (now - last_counter > time_counter_hz() / 5) {
                last_counter = now;
                last_jiffies = j;
            }
        }

        __asm__ volatile("wfi");
    }
}
