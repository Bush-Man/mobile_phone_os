/*
 * syscall.c - svc dispatch and the phase-5 syscall table.
 *
 * Path: user executes `svc #0` -> sync vector -> exceptions_handler
 * sees EC=0x15 from EL0 -> syscall_dispatch(tf). The ABI puts the
 * number in x8, arguments in x0..x5, result in x0 (negative errno
 * on failure). Everything user-pointer-ish goes through uaccess.
 *
 * fork and sigreturn need the raw trap frame (they manufacture or
 * consume whole register states), so they are special-cased before
 * the generic table lookup.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "exceptions.h"
#include "irq.h"
#include "lib.h"
#include "mm/types.h"
#include "panic.h"
#include "proc.h"
#include "signal.h"
#include "syscall.h"
#include "task.h"
#include "time.h"
#include "uaccess.h"
#include "uart.h"

typedef long (*sys_fn_t)(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t);

/* ---- console ------------------------------------------------------------ */

#define IO_CHUNK 256

static long sys_write(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    char kbuf[IO_CHUNK];
    size_t left = (size_t)len;
    uint64_t off = 0;

    if (fd != 1 && fd != 2)
        return -EBADF;

    while (left) {
        size_t chunk = left < sizeof(kbuf) ? left : sizeof(kbuf);

        if (uacc_copy_in_cur(kbuf, (const void *)(uintptr_t)(ubuf + off),
                             chunk))
            return -EFAULT;
        for (size_t i = 0; i < chunk; i++)
            uart_putc(kbuf[i]);
        off += chunk;
        left -= chunk;
    }
    return (long)len;
}

static long sys_read(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    char kbuf[IO_CHUNK];
    unsigned n;

    if (fd != 0)
        return -EBADF;
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);

    /* drain whatever the interrupt-fed RX ring holds (non-blocking) */
    {
        daif_state s = irq_local_save();

        n = uart_rx_read(kbuf, (unsigned)len);
        irq_local_restore(s);
    }

    if (!n)
        return -EAGAIN;                 /* would block; no blocking io yet */
    if (uacc_copy_out_cur((void *)(uintptr_t)ubuf, kbuf, n))
        return -EFAULT;
    return (long)n;
}

/* ---- process ------------------------------------------------------------ */

static long sys_getpid(void)
{
    struct proc *p = proc_current();

    return p ? p->pid : -EINVAL;
}

static long sys_exit(uint64_t code)
{
    proc_do_exit((int)(code & 0xff));   /* never returns */
}

static long sys_execve(uint64_t uname, uint64_t uargv, uint64_t uenvp)
{
    static const char *const none[] = { NULL };

    char name[PROC_NAME_MAX];
    const char *argv_copy[8];
    const char *envp_copy[4];
    char argv_store[8][48];
    char envp_store[2][32];
    unsigned i;

    /*
     * execve replaces the address space, so argv/envp strings must
     * be copied OUT of the dying space first. Caps are deliberate:
     * the built-in image set is tiny.
     */
    {
        long nl = uacc_strnlen_user_cur(uname, sizeof(name) - 1);

        if (nl < 0 || !nl)
            return -ENOENT;
        if (uacc_copy_in_cur(name, (const void *)(uintptr_t)uname,
                             (size_t)nl))
            return -EFAULT;
        name[nl] = 0;
    }

    for (i = 0; i < 8; i++) {
        uint64_t ap;

        if (!uargv)
            break;
        if (uacc_copy_in_cur(&ap, (const void *)(uintptr_t)
                             (uargv + i * sizeof(uint64_t)),
                             sizeof(ap)))
            return -EFAULT;
        if (!ap)
            break;
        {
            long sl = uacc_strnlen_user_cur(ap, sizeof(argv_store[i]) - 1);
            long r;

            if (sl < 0)
                return -EFAULT;
            r = uacc_copy_in_cur(argv_store[i], (const void *)(uintptr_t)ap,
                                 (size_t)sl);
            if (r)
                return -EFAULT;
            argv_store[i][sl] = 0;
            argv_copy[i] = argv_store[i];
        }
    }
    argv_copy[i] = NULL;

    if (uenvp) {
        for (i = 0; i < 1; i++) {
            uint64_t ep;
            long sl, r;

            if (uacc_copy_in_cur(&ep, (const void *)(uintptr_t)uenvp,
                                 sizeof(ep)))
                return -EFAULT;
            if (!ep)
                break;
            sl = uacc_strnlen_user_cur(ep, sizeof(envp_store[i]) - 1);
            if (sl < 0)
                return -EFAULT;
            r = uacc_copy_in_cur(envp_store[i], (const void *)(uintptr_t)ep,
                                 (size_t)sl);
            if (r)
                return -EFAULT;
            envp_store[i][sl] = 0;
            envp_copy[i] = envp_store[i];
        }
        envp_copy[i] = NULL;
    } else {
        envp_copy[0] = NULL;
    }

    return proc_do_exec(name, argv_copy,
                        uenvp ? envp_copy : none);
}

static long sys_waitpid(uint64_t want)
{
    int code;
    int pid = proc_do_waitpid((int)want, &code);

    if (pid < 0)
        return pid;
    /* compact form: exit code in the low byte, pid above it */
    return (long)((code & 0xff) | ((uint64_t)pid << 8));
}

static long sys_kill(uint64_t pid, uint64_t sig)
{
    return proc_do_kill((int)pid, (unsigned)sig);
}

static long sys_sigaction(uint64_t sig, uint64_t handler)
{
    struct proc *p = proc_current();

    if (!p || sig == 0 || sig >= NSIG)
        return -EINVAL;
    if (sig == SIGKILL || sig == SIGCHLD)
        return -EINVAL;                 /* not catchable here */
    p->sig_handler[sig] =
        (sig_handler_t)(uintptr_t)(handler ? handler : 0);
    return 0;
}

static long sys_sleep(uint64_t msecs)
{
    msleep(msecs ? msecs : 1);
    return 0;
}

static long sys_uptime_ms(void)
{
    return (long)time_uptime_ms();
}

/* ---- table + dispatch ------------------------------------------------------ */

static const sys_fn_t sys_table[] = {
    [SYS_exit]       = (sys_fn_t)sys_exit,
    [SYS_fork]       = NULL,            /* special-cased (needs tf)   */
    [SYS_execve]     = (sys_fn_t)sys_execve,
    [SYS_waitpid]    = (sys_fn_t)sys_waitpid,
    [SYS_write]      = (sys_fn_t)sys_write,
    [SYS_read]       = (sys_fn_t)sys_read,
    [SYS_getpid]     = (sys_fn_t)sys_getpid,
    [SYS_kill]       = (sys_fn_t)sys_kill,
    [SYS_sigreturn]  = NULL,            /* special-cased (needs tf)   */
    [SYS_sleep]      = (sys_fn_t)sys_sleep,
    [SYS_sigaction]  = (sys_fn_t)sys_sigaction,
    [SYS_uptime_ms]  = (sys_fn_t)sys_uptime_ms,
};

void syscall_dispatch(struct trap_frame *tf)
{
    uint64_t nr = tf->regs[8];

    switch (nr) {
    case SYS_fork:
        tf->regs[0] = (uint64_t)proc_do_fork(tf);
        return;
    case SYS_sigreturn:
        proc_sigreturn(tf);             /* may kill us; else restores */
        tf->regs[0] = 0;
        return;
    default:
        break;
    }

    if (nr < ARRAY_SIZE(sys_table) && sys_table[nr]) {
        tf->regs[0] = (uint64_t)(long)sys_table[nr](
            tf->regs[0], tf->regs[1], tf->regs[2],
            tf->regs[3], tf->regs[4], tf->regs[5]);
        return;
    }

    kprintf("[proc] pid %d unknown syscall %llu\n",
            proc_current() ? proc_current()->pid : -1,
            (unsigned long long)nr);
    tf->regs[0] = (uint64_t)-ENOSYS;
}
