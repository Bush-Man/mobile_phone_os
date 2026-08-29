/*
 * pthread.c - pthread-lite over SYS_clone/SYS_thread_exit (phase 14).
 *
 * A thread is a kernel task sharing the leader's proc: same address
 * space, fd table, pid. pthread_create mmaps a 64 KiB stack, parks
 * a control block {fn, arg, retval, done} INSIDE it and enters the
 * kernel with a small trampoline, so the tid never needs to cross
 * back into the joiner: join just polls the shared `done` flag.
 * Same-process sharing is what makes that correct -- and it is the
 * scope: exec-while-threaded is refused by the kernel (phase 14
 * note in proc.c), and threads of one process are the phone's
 * whole concurrency story for now.
 *
 * Atomics are integer (LDAXRB/STXRB) -- fine under
 * -mgeneral-regs-only, which only excludes FP/SIMD.
 */

#include "libc.h"

#define THREAD_STACK (64u * 1024u)

struct tcb {
    void *(*fn)(void *);
    void *arg;
    void *retval;
    volatile int done;
    void *stack_base;           /* for a future free-on-join         */
};

struct iarg {                   /* passed to the trampoline in x0    */
    struct tcb *tcb;
    volatile int armed;         /* set before SYS_clone returns...   */
};

/*
 * Wait until a thread slot's `done` flips. The 1 ms sleep keeps the
 * polling join from starving its peer on a single cpu.
 */
static void spin_until(volatile int *flag)
{
    while (!*flag)
        sleep_ms(1);
}

static void thread_trampoline(struct iarg *ia)
{
    struct tcb *t = ia->tcb;

    spin_until(&ia->armed);
    t->retval = t->fn(t->arg);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    _sys0(SYS_thread_exit);     /* parks this task for good          */
    for (;;)
        ;
}

int pthread_create(pthread_t *t_out, void *attr,
                   void *(*fn)(void *), void *arg)
{
    struct tcb *t;
    struct iarg ia;
    i64 tid;

    (void)attr;
    if (!t_out || !fn)
        return -1;

    t = mmap_anon(THREAD_STACK);
    if (t == (void *)-1 || !t)
        return -1;

    t->fn = fn;
    t->arg = arg;
    t->retval = NULL;
    t->done = 0;
    t->stack_base = t;

    /* iarg lives on OUR stack: the trampoline spins on `armed`
     * until we publish this tcb, then runs for real              */
    ia.tcb = t;
    ia.armed = 0;

    tid = _sys3(SYS_clone, (i64)thread_trampoline,
                (i64)((char *)t + THREAD_STACK), (i64)&ia);
    if (tid < 0) {
        return -1;
    }
    ia.armed = 1;
    *t_out = (pthread_t)t;
    return 0;
}

int pthread_join(pthread_t th, void **retval_out)
{
    struct tcb *t = (struct tcb *)th;

    if (!t)
        return -1;
    spin_until(&t->done);
    if (retval_out)
        *retval_out = t->retval;
    return 0;
}

void pthread_exit(void *retval)
{
    (void)retval;               /* retval propagation needs the
                                 * tcb; plain threads park here     */
    _sys0(SYS_thread_exit);
    for (;;)
        ;
}

void pthread_mutex_lock(pthread_mutex_t *m)
{
    while (__atomic_test_and_set(&m->v, __ATOMIC_ACQUIRE))
        sleep_ms(1);            /* back off instead of spinning hot  */
}

void pthread_mutex_unlock(pthread_mutex_t *m)
{
    __atomic_store_n(&m->v, 0, __ATOMIC_RELEASE);
}
