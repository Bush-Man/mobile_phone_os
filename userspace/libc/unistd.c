/*
 * unistd.c - thin wrappers over the raw ABI + the phase-14 report
 * calls (psinfo/mountinfo/netinfo/battinfo/devlist, get/settime,
 * poweroff/reboot). Everything here is one svc deep on purpose:
 * the libc adds no policy, only types.
 */

#include "libc.h"

/* raw svc trampolines: number in x8, args in x0..x5, result in x0 */
i64 _sys3(i64 n, i64 a, i64 b, i64 c)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;

    __asm__ volatile ("svc #0"
                      : "+r"(x0)
                      : "r"(x8), "r"(x0), "r"(x1), "r"(x2)
                      : "memory", "cc");
    return x0;
}

i64 _sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d;
    register i64 x4 __asm__("x4") = e;
    register i64 x5 __asm__("x5") = f;

    __asm__ volatile ("svc #0"
                      : "+r"(x0)
                      : "r"(x8), "r"(x0), "r"(x1), "r"(x2),
                        "r"(x3), "r"(x4), "r"(x5)
                      : "memory", "cc");
    return x0;
}

i64 _sys0(i64 n)
{
    return _sys3(n, 0, 0, 0);
}

i64 _sys1(i64 n, i64 a)
{
    return _sys3(n, a, 0, 0);
}

i64 _sys2(i64 n, i64 a, i64 b)
{
    return _sys3(n, a, b, 0);
}

void _exit(int code)
{
    _sys1(SYS_exit, code);
    for (;;)
        ;
}

i64 read(int fd, void *buf, size_t n)
{
    return _sys3(SYS_read, fd, (i64)buf, (i64)n);
}

i64 write(int fd, const void *buf, size_t n)
{
    return _sys3(SYS_write, fd, (i64)buf, (i64)n);
}

int open(const char *path, unsigned flags)
{
    return (int)_sys2(SYS_open, (i64)path, flags);
}

int close(int fd)
{
    return (int)_sys1(SYS_close, fd);
}

i64 lseek(int fd, i64 off, int whence)
{
    return _sys3(SYS_lseek, fd, off, whence);
}

int unlink(const char *path)
{
    return (int)_sys1(SYS_unlink, (i64)path);
}

int mkdir(const char *path)
{
    return (int)_sys1(SYS_mkdir, (i64)path);
}

/* arg is u64 so callers can pass pointers or plain values alike   */
int ioctl(int fd, u64 cmd, u64 arg)
{
    return (int)_sys3(SYS_ioctl, fd, (i64)cmd, (i64)arg);
}

int getpid(void)
{
    return (int)_sys0(SYS_getpid);
}

int fork(void)
{
    return (int)_sys0(SYS_fork);
}

int execve(const char *name, char *const argv[], char *const envp[])
{
    return (int)_sys3(SYS_execve, (i64)name, (i64)argv, (i64)envp);
}

/* compact kernel form: (code & 0xff) | (pid << 8) on success       */
i64 waitpid(int want)
{
    return _sys1(SYS_waitpid, want);
}

int kill(int pid, int sig)
{
    return (int)_sys2(SYS_kill, pid, sig);
}

sighandler_t sigaction(int sig, sighandler_t h)
{
    return (sighandler_t)(uintptr_t)_sys2(SYS_sigaction, sig,
                                          (i64)(uintptr_t)h);
}

void sleep_ms(u64 ms)
{
    _sys1(10 /* SYS_sleep */, (i64)(ms ? ms : 1));
}

u64 uptime_ms(void)
{
    return (u64)_sys0(SYS_uptime_ms);
}

void *mmap_anon(size_t len)
{
    return (void *)(uintptr_t)_sys2(SYS_mmap, 0, (i64)len);
}

int shmget(u64 npages)
{
    return (int)_sys1(SYS_shmget, (i64)npages);
}

void *shmat(int id)
{
    return (void *)(uintptr_t)_sys2(SYS_shmat, id, 0);
}

int shmdt(void *va)
{
    return (int)_sys1(SYS_shmdt, (i64)va);
}

int usock_serve(const char *path)
{
    return (int)_sys1(SYS_usock_serve, (i64)path);
}

int usock_connect(const char *path)
{
    return (int)_sys1(SYS_usock_connect, (i64)path);
}

int usock_accept(int lfd)
{
    return (int)_sys1(SYS_usock_accept, lfd);
}

/* ---- phase-14 report calls ----------------------------------------- */

int psinfo(void *ents, unsigned max)
{
    return (int)_sys2(SYS_psinfo, (i64)ents, max);
}

int mountinfo(void *ents, unsigned max)
{
    return (int)_sys2(SYS_mountinfo, (i64)ents, max);
}

int mount_fs(const char *fstype, const char *path)
{
    return (int)_sys2(SYS_mount, (i64)fstype, (i64)path);
}

int netinfo(void *ents, unsigned max)
{
    return (int)_sys2(SYS_netinfo, (i64)ents, max);
}

int battinfo(void *info)
{
    return (int)_sys1(SYS_battinfo, (i64)info);
}

int devlist(void *ents, unsigned max)
{
    return (int)_sys2(SYS_devlist, (i64)ents, max);
}

u64 gettime_ns(void)
{
    return (u64)_sys0(SYS_gettime);
}

int settime_ns(u64 epoch_ns)
{
    return (int)_sys1(SYS_settime, (i64)epoch_ns);
}

int poweroff(void)
{
    return (int)_sys0(SYS_poweroff);
}

int reboot(void)
{
    return (int)_sys0(SYS_reboot);
}
