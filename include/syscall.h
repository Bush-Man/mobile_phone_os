#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

struct trap_frame;

/*
 * Phase-5 syscall ABI (Linux-flavoured subset):
 *
 *   number in x8, arguments x0..x5, return value in x0.
 *   Errors are returned as negative errno values (no errno var).
 */
#define SYS_exit       1
#define SYS_fork       2
#define SYS_execve     3
#define SYS_waitpid    4
#define SYS_write      5
#define SYS_read       6
#define SYS_getpid     7
#define SYS_kill       8
#define SYS_sigreturn  9
#define SYS_sleep     10
#define SYS_sigaction 11
#define SYS_uptime_ms 12

/* minimal errno subset (values match Linux/aarch64) */
#define EPERM   1
#define ESRCH   3
#define ENOENT  2
#define EBADF   9
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define ECHILD 10
#define EBUSY  16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOSPC 28
#define ENOSYS 38
#define ENOTEMPTY 39
/* phase 7: filesystem-oriented extensions */
#define EIO     5
#define ENXIO   6
#define ESPIPE 29
#define EROFS  30
#define ENAMETOOLONG 36

/* sync-exception entry point (EC == 0x15, from EL0) */
void syscall_dispatch(struct trap_frame *tf);

#endif /* SYSCALL_H */
