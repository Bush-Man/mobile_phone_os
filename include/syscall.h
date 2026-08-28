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
/* phase 7: filesystem access */
#define SYS_open      13
#define SYS_close     14
#define SYS_lseek     15
#define SYS_getdents  16
#define SYS_mkdir     17
#define SYS_rmdir     18
#define SYS_unlink    19
/* phase 8: IPC, sync and the stabilized POSIX-ish surface */
#define SYS_ioctl     20
#define SYS_mmap      21
#define SYS_shmget    22
#define SYS_shmat     23
#define SYS_shmdt     24
#define SYS_pipe      25
#define SYS_dup       26
#define SYS_poll      27
#define SYS_msgget    28
#define SYS_msgsnd    29
#define SYS_msgrcv    30
#define SYS_socketpair 31
#define SYS_usock_serve 32
#define SYS_usock_connect 33
/* phase 11: AF_INET sockets + select */
#define SYS_socket 34
#define SYS_connect 35
#define SYS_bind 36
#define SYS_listen 37
#define SYS_accept 38
#define SYS_send 39
#define SYS_recv 40
#define SYS_select 41

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
#define ENFILE 23
#define EPIPE  32
#define ENOSPC 28
#define ENOSYS 38
#define ENOTEMPTY 39
/* phase 7: filesystem-oriented extensions */
#define EIO     5
#define ENXIO   6
#define ESPIPE 29
#define EROFS  30
#define ENAMETOOLONG 36
/* phase 8: IPC extensions */
#define EPIPE  32
#define EMSGSIZE 90
#define ENOBUFS 105
#define EADDRINUSE 98
#define ENOTCONN 107
#define ENOTSOCK 88
#define ECONNREFUSED 111

/* sync-exception entry point (EC == 0x15, from EL0) */
void syscall_dispatch(struct trap_frame *tf);

#endif /* SYSCALL_H */
