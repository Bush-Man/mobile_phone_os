/*
 * libc.h - the phase-14 userspace libc (single master header).
 *
 * A small C library for EL0 programs, everything syscall-backed:
 * string/memory routines, a printf family writing to fds, a
 * brk-backed malloc, thin wrappers over the raw ABI (unistd) and a
 * pthread-lite built on SYS_clone/SYS_thread_exit.
 *
 * Programs include this one header and link the libc objects
 * (crt0 first, so _start owns the entry); see the Makefile phase-14
 * rules. Freestanding C17, no FP/SIMD, -nostdlib.
 */

#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>
#include <stdint.h>

typedef signed long    i64;
typedef unsigned long  u64;
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)

/* ---- raw syscall ABI (number in x8, args x0..x5, -errno returns) -- */

#define SYS_exit       1
#define SYS_fork       2
#define SYS_execve     3
#define SYS_waitpid    4
#define SYS_write      5
#define SYS_read       6
#define SYS_getpid     7
#define SYS_kill       8
#define SYS_sigreturn  9
#define SYS_sigaction 11
#define SYS_uptime_ms 12
#define SYS_open      13
#define SYS_close     14
#define SYS_lseek     15
#define SYS_getdents  16
#define SYS_mkdir     17
#define SYS_rmdir     18
#define SYS_unlink    19
#define SYS_ioctl     20
#define SYS_mmap      21
#define SYS_shmget    22
#define SYS_shmat     23
#define SYS_shmdt     24
#define SYS_pipe      25
#define SYS_dup       26
#define SYS_usock_serve   32
#define SYS_usock_connect 33
#define SYS_socket    34
#define SYS_connect   35
#define SYS_send      39
#define SYS_recv      40
#define SYS_brk       42
#define SYS_psinfo    43
#define SYS_mountinfo 44
#define SYS_mount     45
#define SYS_netinfo   46
#define SYS_battinfo  47
#define SYS_gettime   48
#define SYS_settime   49
#define SYS_devlist   50
#define SYS_clone     51
#define SYS_thread_exit 52
#define SYS_poweroff  53
#define SYS_reboot    54

/* minimal errno subset (mirrors the kernel header) */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EIO      5
#define ENXIO    6
#define EBADF    9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ESPIPE  29
#define EROFS   30
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39
#define ENOTSOCK 88
#define EPIPE   32
#define ECONNREFUSED 111

/* open(2)-style flags (values match the kernel vfs.h ABI) */
#define O_RDONLY   0u
#define O_WRONLY   1u
#define O_RDWR     2u
#define O_CREAT  0x040u
#define O_EXCL   0x080u
#define O_TRUNC  0x200u
#define O_APPEND 0x400u

/* lseek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* signals (subset; values match the kernel) */
#define SIGKILL  9
#define SIGUSR1 10
#define SIGTERM 15
#define SIGCHLD 17

typedef void (*sighandler_t)(int);

i64 _sys0(i64 n);
i64 _sys1(i64 n, i64 a);
i64 _sys2(i64 n, i64 a, i64 b);
i64 _sys3(i64 n, i64 a, i64 b, i64 c);
i64 _sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f);

/* ---- string/memory (freestanding set) ------------------------------ */

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
unsigned long strtoul(const char *s, const char **end_out, int base);

/* ---- stdio: printf family over the write() path -------------------- */

int    printf(const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    snprintf(char *buf, size_t cap, const char *fmt, ...);
int    vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int    puts(const char *s);
int    putchar(int c);

/* ---- malloc: brk-backed free lists --------------------------------- */

void  *malloc(size_t n);
void  *calloc(size_t nm, size_t sz);
void  *realloc(void *p, size_t n);
void   free(void *p);

/* ---- unistd-ish wrappers + phase-14 report calls -------------------- */

void   _exit(int code) __attribute__((noreturn));
i64    read(int fd, void *buf, size_t n);
i64    write(int fd, const void *buf, size_t n);
int    open(const char *path, unsigned flags);
int    close(int fd);
i64    lseek(int fd, i64 off, int whence);
int    unlink(const char *path);
int    mkdir(const char *path);
int    getpid(void);
int    fork(void);
int    execve(const char *name, char *const argv[], char *const envp[]);
i64    waitpid(int want);           /* compact form: pid<<8 | code   */
int    kill(int pid, int sig);
sighandler_t sigaction(int sig, sighandler_t h);
void   sleep_ms(u64 ms);
u64    uptime_ms(void);
void  *mmap_anon(size_t len);        /* SYS_mmap, private anonymous   */
char  *sbrk(i64 incr);               /* malloc's growth primitive     */
int    shmget(u64 npages);           /* create shm object -> id       */
void  *shmat(int id);                /* map into this process         */
int    shmdt(void *va);
int    usock_serve(const char *path);    /* publish listener fd       */
int    usock_connect(const char *path);  /* connect -> fd             */

int    psinfo(void *ents, unsigned max);
int    mountinfo(void *ents, unsigned max);
int    mount_fs(const char *fstype, const char *path);
int    netinfo(void *ents, unsigned max);
int    battinfo(void *info);
int    devlist(void *ents, unsigned max);
u64    gettime_ns(void);
int    settime_ns(u64 epoch_ns);
int    poweroff(void);
int    reboot(void);

/* ---- pthread-lite -------------------------------------------------- */

typedef unsigned long pthread_t;
typedef struct { volatile int v; } pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

/* fn returns void*: the retval lands where pthread_join can see it */
int  pthread_create(pthread_t *t_out, void *attr,
                    void *(*fn)(void *), void *arg);
int  pthread_join(pthread_t t, void **retval_out);
void pthread_exit(void *retval) __attribute__((noreturn));
void pthread_mutex_lock(pthread_mutex_t *m);
void pthread_mutex_unlock(pthread_mutex_t *m);

#endif /* LIBC_H */

