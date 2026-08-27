/*
 * input.c - event stream core + /dev/event0 + key repeater (ph. 9).
 *
 * One lock-protected ring buffers every event the system produces;
 * blocking readers sleep on a waitqueue whose wake rides the same
 * two-lock discipline as the IPC stack (input_lock -> task_state).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "chardev.h"
#include "irq.h"
#include "lib.h"
#include "pm.h"
#include "spinlock.h"
#include "task.h"
#include "time.h"
#include "input.h"

/* shared scheduler plumbing (kernel/task.c, public symbols)        */
extern spinlock_t task_state_lock;
uint64_t task_next_key(void);

#define INPUT_RING_SZ   256u

static struct {
    struct input_event ring[INPUT_RING_SZ];
    volatile unsigned head, count;

    spinlock_t lock;
    struct waitqueue rdq;

    /* repeater state (single active key is plenty)                 */
    struct {
        bool     down;
        uint16_t code;
        uint64_t t_down_ms;
        uint64_t next_repeat_ms;
    } rep;

    struct input_event_ring stats;
} inp = {
    .lock = SPINLOCK_INIT,
};

static const char ev0_name[] = "event0";

static int ev0_read(struct char_dev *cd, char *dst, unsigned max);
static unsigned ev0_poll(struct char_dev *cd);

static struct char_dev ev0_chardev = {
    .name  = ev0_name,
    .priv  = &inp,
    .read  = ev0_read,
    .write = NULL,                      /* read-only stream           */
    .poll  = ev0_poll,
};

struct char_dev *input_event_dev(void)
{
    return &ev0_chardev;
}

/* ---- producer -------------------------------------------------------------------- */

void input_push(uint16_t type, uint16_t code, int32_t value)
{
    daif_state s;
    struct input_event *slot;

    spin_lock_irqsave(&inp.lock, &s);

    if (inp.count >= INPUT_RING_SZ) {
        inp.stats.dropped++;
        spin_unlock_irqrestore(&inp.lock, s);
        return;
    }

    slot = &inp.ring[(inp.head + inp.count) % INPUT_RING_SZ];
    slot->ms    = (uint32_t)time_uptime_ms();
    slot->type  = type;
    slot->code  = code;
    slot->value = value;
    inp.count++;
    inp.stats.pushed++;

    /* key repeater bookkeeping                                     */
    if (type == EV_KEY) {
        if (value == 1) {
            inp.rep.down       = true;
            inp.rep.code       = code;
            inp.rep.t_down_ms  = slot->ms;
            inp.rep.next_repeat_ms =
                slot->ms + KEY_REPEAT_DELAY_MS;
        } else if (!value && inp.rep.down &&
                   inp.rep.code == code) {
            inp.rep.down = false;
        }
    }

    spin_unlock_irqrestore(&inp.lock, s);

    /* wake readers under the two-lock ordering                     */
    {
        struct task *cur = current_task();
        bool preempt = false;
        daif_state st;

        spin_lock_irqsave(&task_state_lock, &st);
        while (inp.rdq.head) {
            struct task *t = inp.rdq.head;

            inp.rdq.head = t->wq_next;
            t->wq_next   = NULL;
            t->state     = TASK_READY;
            t->rq_key    = task_next_key();
            if (cur && t->prio < cur->prio)
                preempt = true;
        }
        spin_unlock_irqrestore(&task_state_lock, st);

        if (preempt && this_cpu()->current)
            this_cpu()->need_resched = true;
    }

    /* phase 10: every event counts as power-management activity    */
    pm_input_activity();
}

/* ---- consumer ---------------------------------------------------------------------- */

unsigned input_pending(void)
{
    daif_state s;
    unsigned n;

    spin_lock_irqsave(&inp.lock, &s);
    n = inp.count;
    spin_unlock_irqrestore(&inp.lock, s);
    return n;
}

void input_stats_get(struct input_event_ring *out)
{
    daif_state s;

    spin_lock_irqsave(&inp.lock, &s);
    *out = inp.stats;
    spin_unlock_irqrestore(&inp.lock, s);
}

/* pop up to `max` events without blocking; caller holds the lock  */
static unsigned pop_locked(struct input_event *dst, unsigned max)
{
    unsigned n = 0;

    while (n < max && inp.count) {
        dst[n] = inp.ring[inp.head];
        inp.head = (inp.head + 1u) % INPUT_RING_SZ;
        inp.count--;
        n++;
    }
    inp.stats.read_events += n;
    return n;
}

/*
 * Blocking fill of dst. Empty ring -> park CURRENT on rdq while
 * STILL HOLDING input's own lock (the two-lock dance from
 * kernel/sync.c: subsystem lock -> task_state_lock, no window
 * between "saw empty" and "queued"), so a concurrent input_push()
 * can never lose the wakeup.
 */
static int ev0_read(struct char_dev *cd, char *dst, unsigned max)
{
    struct input_event kbuf[16];
    unsigned want, got;
    daif_state s;

    (void)cd;
    if (!dst || !max)
        return 0;

    want = max / sizeof(kbuf[0]);
    if (!want)
        want = 1;
    if (want > 16)
        want = 16;

    spin_lock_irqsave(&inp.lock, &s);

    got = pop_locked(kbuf, want);
    while (!got && this_cpu()->current) {
        struct per_cpu *pc = this_cpu();
        daif_state st;

        /* park atomically: same invariant as sync.c               */
        spin_lock_irqsave(&task_state_lock, &st);
        pc->current->wq_next = inp.rdq.head;
        inp.rdq.head = pc->current;
        pc->current->state = TASK_BLOCKED;
        spin_unlock_irqrestore(&task_state_lock, st);

        spin_unlock_irqrestore(&inp.lock, s);
        sched_park();                   /* never returns             */

        spin_lock_irqsave(&inp.lock, &s);   /* resumed: retry        */
        got = pop_locked(kbuf, want);
    }

    spin_unlock_irqrestore(&inp.lock, s);

    memcpy(dst, kbuf, (size_t)got * sizeof(kbuf[0]));
    return (int)(got * sizeof(kbuf[0]));
}

static unsigned ev0_poll(struct char_dev *cd)
{
    (void)cd;
    return input_pending() ? 1u : 0u;
}

/* ---- key repeater (plan item 52) ---------------------------------------------------- */

void input_tick_repeats(void)
{
    uint64_t now = time_uptime_ms();
    daif_state s;
    bool fire = false;

    spin_lock_irqsave(&inp.lock, &s);
    if (inp.rep.down && now >= inp.rep.next_repeat_ms) {
        inp.rep.next_repeat_ms += KEY_REPEAT_RATE_MS;
        fire = true;
    }
    spin_unlock_irqrestore(&inp.lock, s);

    /* Linux autorepeat convention: value == 2                      */
    if (fire)
        input_push(EV_KEY, inp.rep.code, 2);
}

/* ---- subsystem init ----------------------------------------------------------------- */

static bool inited;

void input_subsys_init(void)
{
    if (inited)
        return;
    inited = true;

    if (char_dev_register(&ev0_chardev))
        kprintf("input: event0 registration failed\n");
}

