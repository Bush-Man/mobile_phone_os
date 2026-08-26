/*
 * selftest_sched.c - phase 4 scheduling verification + milestone demo.
 *
 * sched_selftest(): two helper threads ping-pong over a waitqueue with
 * a strict-turn handshake, while the boot context polls (IRQs masked
 * so it cannot be disturbed mid-check) until both helpers finish.
 * The completed sequence must be exactly 0,1,2,... -- any lost
 * wakeup, double run or out-of-order switch breaks the arithmetic
 * and panics. The helpers run on whatever cpu picks them up while
 * the boot cpu spins, so handoffs cross cpus.
 *
 * The stall deadline is derived from the raw system counter rather
 * than jiffies so it still fires if timer delivery itself dies.
 *
 * sched_demo_start(): spawns the persistent milestone pair ("ping"/
 * "pong"), printing alternating rounds tagged with the cpu each round
 * ran on; they exit after PP_DEMO_ROUNDS.
 */

#include <stdint.h>
#include <stdbool.h>

#include "cpu.h"
#include "lib.h"
#include "panic.h"
#include "task.h"
#include "time.h"

/* ---- shared ping-pong machinery ------------------------------------------ */

struct turn_wait {
    volatile unsigned *turn;
    unsigned           want;
};

static struct waitqueue pp_wq;

static bool turn_is_mine(void *p)
{
    const struct turn_wait *tw = p;

    return *tw->turn != tw->want;       /* true = keep waiting */
}

static void pp_wait_turn(volatile unsigned *turn, unsigned want)
{
    struct turn_wait tw = { turn, want };

    wait_sleep_when(turn_is_mine, &tw, &pp_wq);
}

/* ---- strict verification pair (selftest) ---------------------------------- */

#define ST_ROUNDS 1000

static volatile unsigned st_turn;
static volatile unsigned st_seq;
static volatile bool     st_done[2];

static void st_helper(void *arg)
{
    unsigned mine = (unsigned)(uintptr_t)arg;

    for (unsigned r = 0; r < ST_ROUNDS; r++) {
        pp_wait_turn(&st_turn, mine);

        if (st_seq != 2u * r + mine)
            panic("selftest: ping-pong order broken");
        st_seq++;
        st_turn = mine ^ 1u;
        wait_wake_all(&pp_wq);
    }
    st_done[mine] = true;
    task_exit();
}

void sched_selftest(void)
{
    const uint64_t deadline_ns = time_uptime_ns() + 10000000000ull;

    if (task_create("pp-a", st_helper, (void *)0UL, 10) < 0 ||
        task_create("pp-b", st_helper, (void *)1UL, 10) < 0)
        panic("selftest: cannot create helpers");

    {
        daif_state s = irq_local_save();

        while (!(st_done[0] && st_done[1])) {
            if (time_uptime_ns() > deadline_ns)
                panic("selftest: scheduler ping-pong stalled");
            __asm__ volatile("nop");
        }
        irq_local_restore(s);
    }

    if (st_seq != 2u * ST_ROUNDS)
        panic("selftest: ping-pong sequence incomplete");

    kprintf("selftest: sched (%u cross-cpu handoffs) ok\n",
            2u * ST_ROUNDS);
}

/* ---- milestone demo pair --------------------------------------------------- */

#define PP_DEMO_ROUNDS 30

static volatile unsigned pp_turn;

static void pp_thread(void *arg)
{
    unsigned mine = (unsigned)(uintptr_t)arg;
    const char *name = mine ? "pong" : "ping";

    for (unsigned r = 0; r < PP_DEMO_ROUNDS; r++) {
        pp_wait_turn(&pp_turn, mine);

        kprintf("%s: round %u on cpu%llu\n",
                name, r + 1, (unsigned long long)cpu_id());
        msleep(4);                      /* stretch the show out */
        pp_turn = mine ^ 1u;
        wait_wake_all(&pp_wq);
    }
    task_exit();
}

void sched_demo_start(void)
{
    if (task_create("ping", pp_thread, (void *)0UL, 10) < 0 ||
        task_create("pong", pp_thread, (void *)1UL, 10) < 0)
        panic("demo: cannot create threads");
}
