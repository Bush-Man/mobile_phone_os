/*
 * sync.c - sleeping mutexes/semaphores + lock debugging (phase 8).
 *
 * Design contract (see include/sync.h):
 *
 *   one core spinlock (`sync_lock`) guards every primitive's owner/
 *   count fields and the debug counters. Lock ordering is strictly
 *   sync_lock BEFORE the scheduler's task_state_lock; nothing blocks
 *   under either.
 *
 * Parking is done by hand instead of through wait_sleep_when()
 * because the wakeup sides must move "release" and "requeue waiters"
 * atomically relative to waiter enqueue: evaluating the condition
 * under sync_lock alone would leave an unlocked interval between the
 * check and the wait-queue insert. Holding both locks across every
 * check/enqueue/release transition removes the lost-wakeup window
 * entirely -- the same reasoning phase 4 applied inside
 * wait_sleep_when(), extended to a two-lock world.
 *
 * The scheduler bookkeeping in wake_batch() mirrors wait_wake_all()
 * (state, FIFO key, priority-preempt flag); duplicating it is
 * deliberate so queue manipulation happens while we are certain no
 * contender is mid-enqueue.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "cpu.h"
#include "irq.h"
#include "lib.h"
#include "panic.h"
#include "spinlock.h"
#include "sync.h"

/* shared scheduler plumbing (kernel/task.c, public symbols) */
extern spinlock_t task_state_lock;
uint64_t task_next_key(void);

/* ---- global plumbing ------------------------------------------------------- */

static spinlock_t sync_lock = SPINLOCK_INIT;

static struct {
    uint64_t acquires;
    uint64_t blocks;
    uint64_t deadlocks_detected;
    uint64_t max_wakeups;
} ss;

/*
 * Live-primitive registry: fixed arrays -- primitives are scarce
 * (a handful per filesystem/IPC object), pooling beats allocation
 * here and keeps sync_debug_dump() trivially safe.
 */
#define SYNC_REG_MAX 32

static struct kmutex *mutex_reg[SYNC_REG_MAX];
static struct ksem  *sem_reg[SYNC_REG_MAX];

static void reg_add(void **reg, void *p)
{
    for (unsigned i = 0; i < SYNC_REG_MAX; i++) {
        if (!reg[i]) {
            reg[i] = p;
            return;
        }
    }
}

static void reg_del(void **reg, void *p)
{
    for (unsigned i = 0; i < SYNC_REG_MAX; i++)
        if (reg[i] == p) {
            reg[i] = NULL;
            return;
        }
}
/* ---- wake batch -------------------------------------------------------------- */

/*
 * Move every parked waiter of ->wq back to READY. sync_lock held;
 * takes/releases task_state_lock internally (ordering respected).
 * Returns how many were woken.
 */
static unsigned wake_batch(struct waitqueue *wq)
{
    struct task *cur = current_task();
    bool preempt = false;
    unsigned n = 0;
    daif_state st;

    spin_lock_irqsave(&task_state_lock, &st);
    while (wq->head) {
        struct task *t = wq->head;

        wq->head = t->wq_next;
        t->wq_next = NULL;
        t->state = TASK_READY;
        t->rq_key = task_next_key();
        n++;
        if (cur && t->prio < cur->prio)
            preempt = true;         /* waiter outranks this cpu   */
    }
    spin_unlock_irqrestore(&task_state_lock, st);

    if ((uint64_t)n > ss.max_wakeups)
        ss.max_wakeups = n;
    if (preempt && this_cpu()->current)
        this_cpu()->need_resched = true;
    return n;
}

/*
 * Enqueue CURRENT onto ->wq and park, with no observable interval:
 * caller holds sync_lock; we take task_state_lock under it, link +
 * mark BLOCKED, then release both and switch away via sched_park().
 */
static void park_on_queue(struct waitqueue *wq)
{
    struct per_cpu *pc = this_cpu();
    daif_state st;

    spin_lock_irqsave(&task_state_lock, &st);
    pc->current->wq_next = wq->head;
    wq->head = pc->current;
    pc->current->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&task_state_lock, st);

    spin_unlock(&sync_lock);

    sched_park();                   /* never returns               */
}

/* ---- deadlock detector --------------------------------------------------------- */

#define DEADLK_VISIT_MAX 16         /* > MAX_TASKS*2 cannot cycle past */

/*
 * Would blocking on `target` close (or join) a cycle? Walks
 * mutex -> owner -> (that task's intended mutex) -> ... . Semaphore
 * waits never populate ->lock_wait, so chains stop naturally there.
 * Called with sync_lock held; reads other tasks' intent fields under
 * it, which is why writers use the same lock.
 */
static bool would_deadlock(const struct kmutex *target)
{
    const struct kmutex *seen[DEADLK_VISIT_MAX];
    const struct kmutex *m = target;
    struct task *me = current_task();
    unsigned nseen = 0;

    for (;;) {
        struct task *owner = m->owner;

        if (!owner)
            return false;           /* a link vanished meanwhile   */
        if (owner == me)
            return true;            /* recursion / self cycle      */

        if (!owner->lock_wait)
            return false;           /* blocked outside our graphs  */

        m = owner->lock_wait;       /* next hop                    */

        if (nseen >= DEADLK_VISIT_MAX)
            return true;            /* bounded walk exhausted =
                                         longer than any acyclic
                                         chain possible here         */
        for (unsigned i = 0; i < nseen; i++)
            if (seen[i] == m)
                return true;
        seen[nseen++] = m;
    }
}


/* ---- mutex -------------------------------------------------------------------- */

void kmutex_init(struct kmutex *m, const char *name)
{
    m->name = name ? name : "kmutex";
    m->owner = NULL;
    m->wq.head = NULL;

    spin_lock(&sync_lock);
    reg_add((void **)mutex_reg, m);
    spin_unlock(&sync_lock);
}

int kmutex_try(struct kmutex *m)
{
    int r = 0;
    daif_state s;

    spin_lock_irqsave(&sync_lock, &s);
    if (m->owner)
        r = -SYNC_EBUSY;
    else {
        m->owner = current_task();
        ss.acquires++;
    }
    spin_unlock_irqrestore(&sync_lock, s);
    return r;
}

int kmutex_lock(struct kmutex *m)
{
    struct task *me = current_task();

    if (!this_cpu()->current)
        panic("kmutex_lock before entering the scheduler");

    for (;;) {
        daif_state s;

        spin_lock_irqsave(&sync_lock, &s);

        if (!m->owner) {
            m->owner = me;
            me->lock_wait = NULL;
            ss.acquires++;
            spin_unlock_irqrestore(&sync_lock, s);
            return 0;
        }

        if (would_deadlock(m)) {
            me->lock_wait = NULL;
            ss.deadlocks_detected++;
            spin_unlock_irqrestore(&sync_lock, s);
            kprintf("sync: deadlock detected around \"%s\" "
                    "(%s would close the cycle)\n",
                    m->name, me->name ? me->name : "?");
            return -SYNC_DEADLK;
        }

        /*
         * Publish intent first (under sync_lock -- visible to any
         * concurrent detector), then enqueue + park with both locks
         * held. On resume the outer loop re-evaluates from scratch.
         */
        me->lock_wait = m;
        ss.blocks++;
        park_on_queue(&m->wq);      /* drops sync_lock, parks      */
    }
}

void kmutex_unlock(struct kmutex *m)
{
    daif_state s;

    spin_lock_irqsave(&sync_lock, &s);

    if (m->owner != current_task()) {
        spin_unlock_irqrestore(&sync_lock, s);
        kprintf("sync: %s released by non-owner (\"%s\")\n",
                __func__, m->name);
        return;
    }

    m->owner = NULL;
    wake_batch(&m->wq);             /* contenders race fairly      */

    spin_unlock_irqrestore(&sync_lock, s);
}

bool kmutex_owned_by_current(const struct kmutex *m)
{
    struct task *t = current_task();

    return t != NULL && m->owner == t;
}

struct task *kmutex_first_waiter(const struct kmutex *m)
{
    struct task *t;
    daif_state s;

    spin_lock_irqsave(&sync_lock, &s);
    t = m->wq.head;
    spin_unlock_irqrestore(&sync_lock, s);
    return t;
}

/* ---- semaphore ------------------------------------------------------------------ */

void ksem_init(struct ksem *s, const char *name,
               long initial, long max)
{
    s->name = name ? name : "ksem";
    s->max = max < 1 ? 1 : max;
    s->count = initial < 0 ? 0 :
               (initial > s->max ? s->max : initial);
    s->wq.head = NULL;

    spin_lock(&sync_lock);
    reg_add((void **)sem_reg, s);
    spin_unlock(&sync_lock);
}

void ksem_wait(struct ksem *s)
{
    if (!this_cpu()->current)
        panic("ksem_wait before entering the scheduler");

    for (;;) {
        daif_state st;

        spin_lock_irqsave(&sync_lock, &st);

        if (s->count > 0) {
            s->count--;
            ss.acquires++;
            spin_unlock_irqrestore(&sync_lock, st);
            return;
        }

        /*
         * Semaphores contribute no detector edge: owners are not
         * individually tracked, so their waits stay invisible to
         * the mutex graph (documented design limit).
         */
        ss.blocks++;
        park_on_queue(&s->wq);      /* drops sync_lock, parks      */
    }
}

int ksem_trywait(struct ksem *s)
{
    int r = 0;
    daif_state st;

    spin_lock_irqsave(&sync_lock, &st);
    if (s->count <= 0) {
        r = -11;                    /* -EAGAIN (syscall.h value)   */
    } else {
        s->count--;
        ss.acquires++;
    }
    spin_unlock_irqrestore(&sync_lock, st);
    return r;
}

void ksem_post(struct ksem *s)
{
    daif_state st;

    spin_lock_irqsave(&sync_lock, &st);
    if (s->count < s->max)
        s->count++;
    wake_batch(&s->wq);
    spin_unlock_irqrestore(&sync_lock, st);
}

/* ---- debug ------------------------------------------------------------------------ */

static void owner_label(const struct kmutex *m, char *buf, size_t cap)
{
    if (!m->owner || !m->owner->name)
        kstrlcpy(buf, "-", cap);
    else
        kstrlcpy(buf, m->owner->name, cap);
}

void sync_stats_get(struct sync_stats *out)
{
    daif_state s;

    spin_lock_irqsave(&sync_lock, &s);
    out->acquires = ss.acquires;
    out->blocks = ss.blocks;
    out->deadlocks_detected = ss.deadlocks_detected;
    out->max_wakeups = ss.max_wakeups;
    spin_unlock_irqrestore(&sync_lock, s);
}

void sync_debug_dump(void)
{
    daif_state s;

    spin_lock_irqsave(&sync_lock, &s);
    for (unsigned i = 0; i < SYNC_REG_MAX; i++) {
        char who[TASK_NAME_MAX];

        if (mutex_reg[i]) {
            owner_label(mutex_reg[i], who, sizeof(who));
            kprintf("sync: mtx %-14s owner=%-12s\n",
                    mutex_reg[i]->name, who);
        }
    }
    for (unsigned i = 0; i < SYNC_REG_MAX; i++)
        if (sem_reg[i])
            kprintf("sync: sem %-14s count=%ld/%ld\n",
                    sem_reg[i]->name, sem_reg[i]->count,
                    sem_reg[i]->max);
    kprintf("sync: stats acq=%llu blk=%llu dl=%llu wake<=%llu\n",
            (unsigned long long)ss.acquires,
            (unsigned long long)ss.blocks,
            (unsigned long long)ss.deadlocks_detected,
            (unsigned long long)ss.max_wakeups);
    spin_unlock_irqrestore(&sync_lock, s);
}
