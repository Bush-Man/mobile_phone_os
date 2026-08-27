#ifndef SYNC_H
#define SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "task.h"

/*
 * Blocking sync primitives (phase 8, plan item 43).
 *
 * The fast spinlocks from phase 4 stay exactly as they are: they are
 * the right tool for IRQ-adjacent short critical sections. This layer
 * adds SLEEPING primitives -- a non-recursive mutex and a bounded
 * counting semaphore -- for anything that may block underneath the
 * lock (block-layer IO, IPC handoff, ...), with built-in
 * lock-debugging:
 *
 *   - owner tracking   : kmutex records which task holds it and the
 *                        waiter task carries its intent in ->lock_wait
 *                        (struct task, see include/task.h);
 *   - deadlock detector: before a contending task parks, the core
 *                        walks mutex->owner->lock_wait chains under
 *                        the core lock; if the chain comes back to a
 *                        mutex already on it (or reaches ourselves),
 *                        kmutex_lock returns -EDEADLK instead of
 *                        hanging, reporting through serial.
 *
 * Lock-ordering contract shared by the whole subsystem:
 *     sync_lock -> (optionally) task_state_lock
 * Nothing that blocks ever runs under either. Every state transition
 * that can wake waiters (kmutex_unlock, ksem_post) therefore takes
 * BOTH locks together so "condition changed" and "waiter enqueued"
 * can never interleave -- there is no lost-wakeup window by design.
 */

#define SYNC_DEADLK (-35)               /* detector fired              */
#define SYNC_EBUSY  (-16)               /* try() hit an active owner   */

/* ---- mutex ------------------------------------------------------------------ */

struct kmutex {
    const char     *name;
    struct task    *owner;          /* NULL = free                  */
    struct waitqueue wq;
};

void kmutex_init(struct kmutex *m, const char *name);

/*
 * Acquire, blocking until available (predicate parking). Returns 0,
 * or -EDEADLK if taking it would close a cycle in the ownership
 * graph (including recursing on a lock we already hold).
 */
int  kmutex_lock(struct kmutex *m);

/* non-blocking acquire: 0 or -EBUSY */
int  kmutex_try(struct kmutex *m);

/* release; must be called by the owner from task context */
void kmutex_unlock(struct kmutex *m);

bool kmutex_owned_by_current(const struct kmutex *m);

/* ---- semaphore ---------------------------------------------------------------- */

struct ksem {
    const char     *name;
    long            count;
    long            max;            /* post() beyond this drops      */
    struct waitqueue wq;
};

void ksem_init(struct ksem *s, const char *name, long initial, long max);

/* P(): block while count == 0, then decrement. Never returns error. */
void ksem_wait(struct ksem *s);

/* non-blocking P(): 0 on success, -EAGAIN when empty */
int  ksem_trywait(struct ksem *s);

/* V(): increment up to max, wake any waiters. */
void ksem_post(struct ksem *s);

/* ---- debug surface ------------------------------------------------------------ */

struct sync_stats {
    uint64_t acquires;              /* successful mutex+sem takes   */
    uint64_t blocks;                /* sleeps parked on primitives  */
    uint64_t deadlocks_detected;    /* detector firings             */
    uint64_t max_wakeups;           /* biggest single wake batch    */
};

void sync_stats_get(struct sync_stats *out);
void sync_debug_dump(void);         /* one line per live primitive  */

#endif /* SYNC_H */