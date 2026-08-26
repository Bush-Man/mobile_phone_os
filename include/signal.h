#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>

/*
 * Signals (phase 5 subset, Linux-compatible numbers).
 *
 * Delivery model: pending bits are checked on every kernel->EL0
 * boundary (syscall return, IRQ return to user, fresh exec). A
 * delivered signal installs a user handler by building a signal
 * frame on the user stack; the handler returns through an embedded
 * trampoline that issues SYS_sigreturn.
 */
#define NSIG 32

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCHLD 17

typedef void (*sig_handler_t)(int);

#define SIG_DFL  ((sig_handler_t)0)     /* default action              */
#define SIG_IGN  ((sig_handler_t)1)     /* ignore                      */

/* default action = terminate the process */
bool signal_default_fatal(unsigned sig);

/*
 * Frame written to the user stack at signal delivery and consumed
 * by SYS_sigreturn. Offsets are self-contained (kernel copies the
 * whole struct around); magic guards against garbage stacks.
 */
struct sigframe {
    uint64_t magic;
    uint64_t regs[31];          /* x0 .. x30                       */
    uint64_t sp;                /* interrupted user SP             */
    uint64_t elr;               /* interrupted user PC             */
    uint64_t spsr;              /* interrupted user PSTATE         */
};

#define SIGFRAME_MAGIC 0x47465653ULL    /* "SVFG" */

#endif /* SIGNAL_H */
