/*
 * exceptions.c - exception vector installation and early fault reporting.
 */

#include <stdint.h>
#include <stddef.h>

#include "cpu.h"
#include "exceptions.h"
#include "irq.h"
#include "lib.h"
#include "mm/types.h"
#include "panic.h"
#include "proc.h"
#include "syscall.h"
#include "task.h"
#include "uart.h"

extern uint8_t vectors_begin[];

/* experiment mode: skip faulting stores instead of halting */
int exp_skip_faults;

void vectors_init(void)
{
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vectors_begin) : "memory");
    __asm__ volatile("isb");
}

static const char *ec_name(uint64_t ec)
{
    switch (ec) {
    case 0x00: return "unknown";
    case 0x15: return "svc (AArch64)";
    case 0x16: return "hvc (AArch64)";
    case 0x20: return "instruction abort, lower EL";
    case 0x21: return "instruction abort, current EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort, lower EL";
    case 0x25: return "data abort, current EL";
    case 0x2c: return "SP alignment fault";
    default:   return "reserved/unclassified";
    }
}

void exceptions_handler(struct trap_frame *tf, unsigned kind)
{
    static const char *kind_name[] = { "sync", "IRQ", "FIQ", "SError" };
    uint64_t esr = tf->esr;
    uint64_t ec = (esr >> 26) & 0x3f;
    uint64_t far;
    bool from_user = (tf->spsr & 0xc) == 0;     /* SPSR.M[3:2] == EL0 */

    /* faults must be able to report even with locks wedged */
    uart_panic_mode();

    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    /*
     * IRQ/FIQ: hand to the interrupt framework, which acks, runs
     * handlers and EOIs every pending line. Preemption happens only
     * afterwards, on the way out (sched_post_irq), so no interrupt
     * is ever left active across a context switch. Before returning
     * to a user process, pending signals get their chance.
     */
    if (kind == EXC_IRQ || kind == EXC_FIQ) {
        struct per_cpu *pc = this_cpu();

        pc->in_irq = true;
        irq_dispatch();
        sched_post_irq();
        pc->in_irq = false;
        if (from_user)
            signal_deliver_pending(tf);
        return;
    }

    if (kind == EXC_SYNC) {
        /* svc from user: the syscall path */
        if (ec == 0x15 && from_user) {
            syscall_dispatch(tf);
            signal_deliver_pending(tf);         /* e.g. kill(self)   */
            return;
        }

        /* aborts/alignment faults from user: SIGSEGV-style kill */
        if (from_user &&
            (ec == 0x20 || ec == 0x21 || ec == 0x22 ||
             ec == 0x24 || ec == 0x25 || ec == 0x2c))
            proc_user_fault(tf, esr, far);      /* never returns     */

        /* an svc issued from inside the kernel is a kernel bug */
        if (ec == 0x15) {
            kprintf("\n--- svc from kernel context (EL1 bug) ---\n");
            kprintf("ELR=%016llx\n", (unsigned long long)tf->elr);
            panic("kernel-internal svc");
        }
    }

    /* experiment mode: skip faulting stores instead of dying */
    if (exp_skip_faults && kind == EXC_SYNC && !from_user &&
        (ec == 0x24 || ec == 0x25)) {
        tf->elr += 4;
        return;
    }

    kprintf("\n--- exception: %s ---\n", kind_name[kind & 3]);
    kprintf("ESR=%016llx EC=%llx (%s)\n",
            (unsigned long long)esr,
            (unsigned long long)ec, ec_name(ec));
    kprintf("ELR=%016llx FAR=%016llx SPSR=%016llx SP=%016llx\n",
            (unsigned long long)tf->elr, (unsigned long long)far,
            (unsigned long long)tf->spsr, (unsigned long long)tf->sp);
    {
        uint64_t tcr, ttb0, ttb1;

        __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
        __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttb0));
        __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttb1));
        kprintf("TCR=%016llx TTBR0=%016llx TTBR1=%016llx\n",
                (unsigned long long)tcr,
                (unsigned long long)ttb0, (unsigned long long)ttb1);
    }
    panic("unexpected exception");
}
