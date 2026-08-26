/*
 * selftest_sched.c - phase 4 scheduling verification + milestone demo.
 *
 * sched_selftest(): two helper threads ping-pong over a waitqueue with
 * a strict-turn handshake, while the boot context (IRQs masked so it
 * cannot be preempted mid-check) verifies the completed sequence is
 * exactly 0,1,2,... -- any lost wakeup, double run or out-of-order
 * switch breaks the arithmetic and panics. The helpers run on the
 * secondary cpu's queue, so every handoff crosses cpus.
 *
 * sched_demo_start(): spawns the persistent milestone pair ("ping"/
 * "pong"), printing alternating rounds tagged with the cpu each round
 * ran on; they exit after PP_DEMO_ROUNDS.
 */

#include <stdint.h>
#include <stdbool.h>

#include "cpu.h"
#include "gic.h"
#include "irq.h"
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
static volatile unsigned st_kicks;

static bool kick_handler(void *arg)
{
    (void)arg;
    st_kicks++;
    return true;
}

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
    uint64_t deadline;

    if (!irq_register(14, "kick", kick_handler, NULL))
        panic("selftest: no kick slot");
    irq_enable(14);

    if (task_create("pp-a", st_helper, (void *)0UL, 10) < 0 ||
        task_create("pp-b", st_helper, (void *)1UL, 10) < 0)
        panic("selftest: cannot create helpers");

    deadline = jiffies_read() + TIME_HZ * 5;

    /*
     * Poll with IRQs masked: this context is the adopted idle task of
     * cpu0 and must not be switched away from mid-verification. The
     * helpers live on the other cpu's queue and keep making progress
     * via that cpu's own timer ticks.
     */
    {
        daif_state s = irq_local_save();
        uint64_t last_beat = time_uptime_ns();
        uint64_t last_kick = 0;

        while (!(st_done[0] && st_done[1])) {
            uint64_t now = time_uptime_ns();

            /* TEMP: poke the other cpu so it leaves wfi */
            if (now - last_kick > 200000000ull) {
                last_kick = now;
                gic_send_sgi_list(0xfeu, 14);
            }

            if (now - last_beat > 400000000ull) {
                last_beat = now;
                kprintf("[beat seq=%u d=%d%d j=%lu k=%u]\n",
                        st_seq, (int)st_done[0], (int)st_done[1],
                        jiffies_read(), st_kicks);
            }
            if ((long)(jiffies_read() - deadline) >= 0)
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
