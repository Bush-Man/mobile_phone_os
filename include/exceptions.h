#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdint.h>

/*
 * Register frame laid out by the assembly vector stubs.
 * Offsets are mirrored exactly in vectors.S -- do not reorder.
 */
struct trap_frame {
    uint64_t regs[31];          /* x0 .. x30 (x30 = LR)         */
    uint64_t esr;               /* ESR_EL1                      */
    uint64_t elr;               /* ELR_EL1                      */
    uint64_t spsr;              /* SPSR_EL1                     */
    uint64_t sp;                /* SP at exception entry        */
};

enum exc_kind {
    EXC_SYNC = 0,
    EXC_IRQ,
    EXC_FIQ,
    EXC_SERROR,
};

void vectors_init(void);
void exceptions_handler(struct trap_frame *tf, unsigned kind);

#endif /* EXCEPTIONS_H */
