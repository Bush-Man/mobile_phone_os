/*
 * hello.c - the phase-5 milestone program.
 *
 * A freestanding static ELF64 binary that runs at EL0 against the
 * raw syscall ABI (number in x8, args in x0..x2 here, negative
 * errno returns). It exercises the whole phase-5 surface:
 *
 *   getpid, uptime_ms, write, sigaction + kill(self) -> handler ->
 *   sigreturn, fork (child reports and exits 7), waitpid, exit 42.
 *
 * The kernel prints the final exit code it reaped -- that print is
 * the milestone proof ("static hello ELF runs at EL0, returns exit
 * code to kernel").
 *
 * Built with -mgeneral-regs-only: the kernel does not save FP/SIMD
 * state across context switches yet (see docs/PHASE_5.md).
 */

typedef unsigned long u64;
typedef long          i64;

#define SYS_exit       1
#define SYS_fork       2
#define SYS_waitpid    4
#define SYS_write      5
#define SYS_getpid     7
#define SYS_kill       8
#define SYS_sigreturn  9
#define SYS_sigaction 11
#define SYS_uptime_ms 12

#define SIGUSR1 10

static i64 syscall3(i64 n, i64 a, i64 b, i64 c)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x0), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static u64 slen(const char *s)
{
    u64 n = 0;

    while (s[n])
        n++;
    return n;
}

static void out(const char *s)
{
    syscall3(SYS_write, 1, (i64)s, (i64)slen(s));
}

static void out_dec(i64 v)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    unsigned long long m;

    buf[i] = 0;
    if (v < 0)
        m = (unsigned long long)(-v);
    else
        m = (unsigned long long)v;
    do {
        buf[--i] = (char)('0' + (m % 10u));
        m /= 10u;
    } while (m);
    if (v < 0)
        buf[--i] = '-';
    out(&buf[i]);
}

static void out_nl(void)
{
    out("\n");
}

static void sig_handler(int sig)
{
    out("hello: SIGUSR1 handler ran (sig=");
    out_dec(sig);
    out(")");
    out_nl();
    syscall3(SYS_sigreturn, 0, 0, 0);
    for (;;)
        ;                               /* never reached */
}

void _start(unsigned long argc, char **argv, char **envp)
{
    i64 pid, child, rc;

    (void)envp;

    out("hello: running at EL0, pid ");
    out_dec(syscall3(SYS_getpid, 0, 0, 0));
    out_nl();
    out("hello: argc=");
    out_dec((i64)argc);
    out(" argv[0]=");
    out(argv && argv[0] ? argv[0] : "(none)");
    out_nl();

    out("hello: uptime ");
    out_dec(syscall3(SYS_uptime_ms, 0, 0, 0));
    out(" ms");
    out_nl();

    /* signals: install a handler, then raise one at ourselves */
    if (syscall3(SYS_sigaction, SIGUSR1, (i64)sig_handler, 0)) {
        out("hello: sigaction failed");
        out_nl();
    } else {
        out("hello: raising SIGUSR1");
        out_nl();
        syscall3(SYS_kill, syscall3(SYS_getpid, 0, 0, 0), SIGUSR1, 0);
    }

    /* processes: fork; child exits 7, parent reaps it */
    child = syscall3(SYS_fork, 0, 0, 0);
    if (child == 0) {
        out("hello-child: pid ");
        out_dec(syscall3(SYS_getpid, 0, 0, 0));
        out(" exiting with 7");
        out_nl();
        syscall3(SYS_exit, 7, 0, 0);
        for (;;)
            ;
    }
    if (child < 0) {
        out("hello: fork failed");
        out_nl();
    } else {
        out("hello: forked child pid ");
        out_dec(child);
        out_nl();
        pid = syscall3(SYS_getpid, 0, 0, 0);
        rc = syscall3(SYS_waitpid, child, 0, 0);
        if (rc < 0) {
            out("hello: waitpid failed");
            out_nl();
        } else {
            out("hello[pid ");
            out_dec(pid);
            out("]: reaped child ");
            out_dec(rc & 0xff);
            out(", code ");
            out_dec(rc >> 8);
            out_nl();
        }
    }

    out("hello: exiting 42");
    out_nl();
    syscall3(SYS_exit, 42, 0, 0);
    for (;;)
        ;
}
