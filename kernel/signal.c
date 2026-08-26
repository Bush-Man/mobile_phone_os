/*
 * signal.c - phase-5 signal machinery.
 *
 * Pending bits are checked at every kernel->EL0 boundary (syscall
 * return, IRQ return to user, right after dispatch). Delivery
 * builds a signal frame on the USER stack containing the complete
 * interrupted user context plus a tiny return trampoline:
 *
 *     [ sigframe ][ pad ][ mov w8,#SYS_sigreturn; svc #0 ]
 *                 ^ handler runs with SP here, growing down
 *                 ^ x30 points at the trampoline above the frame
 *
 * The handler returns into the trampoline, which issues
 * SYS_sigreturn; the kernel then restores the saved context from
 * the frame location recorded in proc->sigframe_va (no reliance on
 * the handler leaving SP anywhere in particular).
 *
 * Nesting is disallowed: while a handler runs, further signals stay
 * pending until sigreturn. Default actions: SIGCHLD ignored,
 * everything unhandled terminates (code 128+sig).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "exceptions.h"
#include "irq.h"
#include "lib.h"
#include "panic.h"
#include "proc.h"
#include "signal.h"
#include "syscall.h"
#include "task.h"
#include "uaccess.h"

bool signal_default_fatal(unsigned sig)
{
    if (sig == 0 || sig >= NSIG)
        return true;
    return sig != SIGCHLD;              /* only SIGCHLD ignores by default */
}

static unsigned first_pending(uint32_t mask)
{
    for (unsigned i = 0; i < 32; i++)
        if (mask & (1u << i))
            return i + 1;
    return 0;
}

/* trampoline: movz w8,#nr ; svc #0 */
static const uint32_t sig_tramp[2] = {
    0xd2800008u | ((uint32_t)SYS_sigreturn << 5),
    0xd4000001u,
};

void signal_deliver_pending(struct trap_frame *tf)
{
    struct proc *p = proc_current();
    unsigned sig;
    sig_handler_t h;
    uint64_t frame_va;

    if (!p || !p->alive)
        return;

    for (;;) {
        sig = first_pending(p->sig_pending);
        if (!sig)
            return;
        p->sig_pending &= ~(1u << (sig - 1));

        h = p->sig_handler[sig];
        if (h == SIG_IGN)
            continue;                   /* swallowed               */
        if (!h || signal_default_fatal(sig))
            proc_die(p, 128 + (int)sig, "killed by signal");

        /*
         * A second signal arriving while a handler runs waits for
         * its sigreturn.
         */
        if (p->in_signal) {
            p->sig_pending |= (1u << (sig - 1));
            return;
        }

        /* place frame + trampoline on the user stack */
        frame_va = ALIGN_DOWN(tf->sp - sizeof(struct sigframe), 16);
        {
            struct sigframe sf;
            uint32_t tramp[2];

            sf.magic = SIGFRAME_MAGIC;
            memcpy(sf.regs, tf->regs, sizeof(sf.regs));
            sf.sp = tf->sp;
            sf.elr = tf->elr;
            sf.spsr = tf->spsr;

            memcpy(tramp, sig_tramp, sizeof(tramp));

            if (uacc_copy_out(p->root_pa, frame_va, &sf, sizeof(sf)) ||
                uacc_copy_out(p->root_pa,
                              frame_va + sizeof(struct sigframe),
                              tramp, sizeof(tramp))) {
                proc_die(p, 128 + SIGSEGV, "signal frame write failed");
            }
        }

        p->in_signal = true;
        p->sigframe_va = frame_va;

        /* redirect execution into the handler */
        tf->regs[0] = sig;
        for (unsigned r = 1; r <= 17; r++)
            tf->regs[r] = 0;            /* caller-saved hygiene       */
        tf->regs[30] = frame_va + sizeof(struct sigframe);
        tf->sp = frame_va;              /* handler grows down from it */
        tf->elr = (uint64_t)(uintptr_t)h;
        return;                         /* one delivery per boundary  */
    }
}

void proc_sigreturn(struct trap_frame *tf)
{
    struct proc *p = proc_current();
    struct sigframe sf;

    if (!p)
        panic("sigreturn outside a process");

    if (!p->in_signal ||
        uacc_copy_in(&sf, p->root_pa, p->sigframe_va, sizeof(sf)) ||
        sf.magic != SIGFRAME_MAGIC ||
        (sf.spsr >> 32) != 0 || (sf.spsr & 0xc) != 0)
        proc_die(p, 128 + SIGSEGV, "bad sigreturn");

    memcpy(tf->regs, sf.regs, sizeof(sf.regs));
    tf->sp = sf.sp;
    tf->elr = sf.elr;
    tf->spsr = sf.spsr;

    p->in_signal = false;
    p->sigframe_va = 0;
}
