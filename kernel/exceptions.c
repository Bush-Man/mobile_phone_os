/*
 * exceptions.c - exception vector installation and early fault reporting.
 */

#include <stdint.h>

#include "exceptions.h"
#include "lib.h"
#include "panic.h"

extern uint8_t vectors_begin[];

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
    case 0x24: return "instruction abort, lower EL";
    case 0x25: return "instruction abort, current EL";
    case 0x26: return "PC alignment fault";
    case 0x2c: return "data abort, lower EL";
    case 0x2f: return "SP alignment fault";
    default:   return "reserved/unclassified";
    }
}

static void dump_regs(const struct trap_frame *tf)
{
    for (unsigned i = 0; i < 31; i += 4) {
        kprintf("  x%-2u=%016llx x%-2u=%016llx "
                "x%-2u=%016llx x%-2u=%016llx\n",
                i,     (unsigned long long)tf->regs[i],
                i + 1, (unsigned long long)(i + 1 < 31 ? tf->regs[i+1] : 0),
                i + 2, (unsigned long long)(i + 2 < 31 ? tf->regs[i+2] : 0),
                i + 3, (unsigned long long)(i + 3 < 31 ? tf->regs[i+3] : 0));
    }
}

void exceptions_handler(struct trap_frame *tf, unsigned kind)
{
    static const char *kind_name[] = { "sync", "IRQ", "FIQ", "SError" };
    uint64_t esr = tf->esr;
    uint64_t ec = (esr >> 26) & 0x3f;
    uint64_t far;

    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    kprintf("\n--- exception: %s ---\n", kind_name[kind & 3]);
    kprintf("ESR=%016llx EC=%llx (%s)\n",
            (unsigned long long)esr,
            (unsigned long long)ec, ec_name(ec));
    kprintf("ELR=%016llx FAR=%016llx SPSR=%016llx SP=%016llx\n",
            (unsigned long long)tf->elr, (unsigned long long)far,
            (unsigned long long)tf->spsr, (unsigned long long)tf->sp);
    dump_regs(tf);

    panic("unexpected exception");
}
