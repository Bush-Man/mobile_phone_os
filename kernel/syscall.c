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
 *
 * Phase 7: read/write moved onto the per-process fd table; fds 0-2
 * are stdio opened on /dev/console when devfs is up. Before that
 * (fds == NULL, e.g. images booted without devfs) the legacy raw
 * UART paths below keep phase-5 semantics byte-for-byte.
 *
 * Phase 8: IPC + multiplexing surface added (pipe/dup/poll, mmap,
 * shm*, msg*, socketpair and the unix transport), plus ioctl for
 * line-discipline switches. Anonymous IPC objects materialize as
 * fd-table entries exactly like regular files, so read/write on
 * them already worked before these numbers existed -- only their
 * CREATION needed syscalls.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "exceptions.h"
#include "chardev.h"
#include "ipc.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "net.h"
#include "panic.h"
#include "proc.h"
#include "signal.h"
#include "syscall.h"
#include "task.h"
#include "time.h"
#include "tty.h"
#include "uaccess.h"
#include "uart.h"
#include "unsock.h"
#include "vfs.h"

typedef long (*sys_fn_t)(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t);

/* ---- console ------------------------------------------------------------ */

#define IO_CHUNK 256

/* legacy fd-less fallbacks (phase 5 behavior)                     */
static long console_write_raw(uint64_t ubuf, size_t left)
{
    char kbuf[IO_CHUNK];
    uint64_t off = 0;

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
    return 0;
}

static long sys_write(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    struct proc *p = proc_current();
    struct file *f;

    if (!p || !p->fds || fd > 2)
        goto legacy;
    if (!(f = vfs_fd_get(p->fds, (int)fd)))
        return -EBADF;

    {
        char kbuf[IO_CHUNK];
        size_t left = (size_t)len;
        uint64_t off = 0;
        long total = 0;

        while (left) {
            size_t chunk = left < sizeof(kbuf) ? left : sizeof(kbuf);
            long r;

            if (uacc_copy_in_cur(kbuf,
                                 (const void *)(uintptr_t)(ubuf + off),
                                 chunk)) {
                file_close(f);
                return total ? total : -EFAULT;
            }
            r = f_write(f, kbuf, chunk);
            if (r <= 0) {
                if (!total)
                    total = r ? r : -EIO;
                break;
            }
            total += r;
            off += r;
            left -= (size_t)r;
            if ((size_t)r < chunk)
                break;                  /* short write                */
        }
        file_close(f);
        return total;
    }

legacy:
    if (fd != 1 && fd != 2)
        return -EBADF;
    {
        long r = console_write_raw(ubuf, (size_t)len);

        return r ? r : (long)len;
    }
}

static long sys_read(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    struct proc *p = proc_current();
    struct file *f;
    char kbuf[IO_CHUNK];
    long r;

    if (!p || !p->fds)
        goto legacy;
    if (!(f = vfs_fd_get(p->fds, (int)fd)))
        return -EBADF;
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);

    /* tty-backed fds block here for a canonical line               */
    r = f_read(f, kbuf, (size_t)len);
    file_close(f);
    if (r <= 0)
        return r ? r : -EAGAIN;
    if (uacc_copy_out_cur((void *)(uintptr_t)ubuf, kbuf, (size_t)r))
        return -EFAULT;
    return r;

legacy:
    if (fd != 0)
        return -EBADF;
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);
    {
        unsigned n;
        daif_state s = irq_local_save();

        n = uart_rx_read(kbuf, (unsigned)len);
        irq_local_restore(s);

        if (!n)
            return -EAGAIN;             /* would block                */
        if (uacc_copy_out_cur((void *)(uintptr_t)ubuf, kbuf, n))
            return -EFAULT;
        return (long)n;
    }
}

/* ---- filesystem ------------------------------------------------------------- */

/* copy a NUL-terminated path in from userland                      */
static int path_copy_in(uint64_t upath, char *kpath /* VFS_PATH_MAX */)
{
    long nl = uacc_strnlen_user_cur((const void *)(uintptr_t)upath,
                                    VFS_PATH_MAX - 1);

    if (nl <= 0)
        return nl < 0 ? -EFAULT : -ENOENT;
    if (uacc_copy_in_cur(kpath, (const void *)(uintptr_t)upath,
                         (size_t)nl))
        return -EFAULT;
    kpath[nl] = '\0';
    return 0;
}

static long sys_open(uint64_t uname, uint64_t flags, uint64_t mode)
{
    struct proc *p = proc_current();
    char kpath[VFS_PATH_MAX];
    struct file *f;
    int fd, r;

    (void)mode;                         /* no permission bits yet     */

    if (!p)
        panic("open outside a process");
    r = path_copy_in(uname, kpath);
    if (r)
        return r;

    r = vfs_open(kpath, (unsigned)flags, &f);
    if (r)
        return r;

    if (!p->fds) {
        r = vfs_proc_fds_init(p);       /* late table: no stdio yet   */
        if (r) {
            file_close(f);
            return r;
        }
    }

    fd = vfs_fd_install(p->fds, f);     /* consumes our reference     */
    if (fd < 0)
        file_close(f);
    return fd;
}

static long sys_close(uint64_t fd)
{
    struct proc *p = proc_current();

    if (!p || !p->fds)
        return -EBADF;
    if (fd >= PROC_FD_MAX)
        return -EBADF;

    /*
     * Stdio stays open for the process lifetime here (no exec-time
     * close-on-exec machinery yet), but an explicit close works.
     */
    {
        struct file *f = vfs_fd_get(p->fds, (int)fd);

        if (!f)
            return -EBADF;
        file_close(f);                  /* drop the get() reference   */
        vfs_fd_put(p->fds, (int)fd);    /* drop the slot's reference  */
    }
    return 0;
}

static long sys_lseek(uint64_t fd, uint64_t off, uint64_t whence)
{
    struct proc *p = proc_current();
    struct file *f;
    int64_t pos;
    int r;

    if (!p || !p->fds || !(f = vfs_fd_get(p->fds, (int)fd)))
        return -EBADF;
    r = f_lseek(f, (int64_t)off, (int)whence, &pos);
    file_close(f);
    return r ? r : (long)pos;
}

static long sys_getdents(uint64_t fd, uint64_t ubuf, uint64_t buflen)
{
    struct proc *p = proc_current();
    struct file *f;
    uint8_t kbuf[512];
    long r;

    if (!p || !p->fds || !(f = vfs_fd_get(p->fds, (int)fd)))
        return -EBADF;
    if (buflen > sizeof(kbuf))
        buflen = sizeof(kbuf);

    r = f_getdents(f, kbuf, (size_t)buflen);
    file_close(f);
    if (r <= 0)
        return r ? r : 0;               /* 0 = end of directory       */
    if (uacc_copy_out_cur((void *)(uintptr_t)ubuf, kbuf, (size_t)r))
        return -EFAULT;
    return r;
}

static long sys_mkdir(uint64_t uname)
{
    char kpath[VFS_PATH_MAX];
    int r = path_copy_in(uname, kpath);

    return r ? r : vfs_mkdir(kpath);
}

static long sys_rmdir(uint64_t uname)
{
    char kpath[VFS_PATH_MAX];
    int r = path_copy_in(uname, kpath);

    return r ? r : vfs_rmdir(kpath);
}

static long sys_unlink(uint64_t uname)
{
    char kpath[VFS_PATH_MAX];
    int r = path_copy_in(uname, kpath);

    return r ? r : vfs_unlink(kpath);
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
        long nl = uacc_strnlen_user_cur((const void *)(uintptr_t)uname,
                                        sizeof(name) - 1);

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
            long sl = uacc_strnlen_user_cur(
                (const void *)(uintptr_t)ap, sizeof(argv_store[i]) - 1);
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
            sl = uacc_strnlen_user_cur((const void *)(uintptr_t)ep,
                                       sizeof(envp_store[i]) - 1);
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

#define ENOTTY 25
/* ---- phase 8: IPC / multiplexing syscalls ---------------------------------- */

#define MMAP_PAGES_MAX  64u             /* 256 KiB per mapping         */

/* console chardev name registered by drivers/tty.c                  */
#define CONSOLE_DEV_NAME "console"

static long sys_uptime_ms(void)
{
    return (long)time_uptime_ms();
}

/*
 * Resolve an fd to its vnode, for ioctl validation.
 * Returns NULL when the fd is not open.
 */
static struct vnode *fd_vnode_of(struct proc *p, int fd)
{
    struct file *f;

    if (!p || !p->fds || fd < 0 || fd >= PROC_FD_MAX)
        return NULL;
    f = vfs_fd_get(p->fds, fd);
    if (!f)
        return NULL;
    return f->vn;
}

static long sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    struct vnode *vn = fd_vnode_of(proc_current(), (int)fd);

    (void)arg;
    if (!vn)
        return -EBADF;
    if (vn->type != V_CHARDEV || !vn->priv ||
        strncmp(((struct char_dev *)vn->priv)->name,
                CONSOLE_DEV_NAME, 8))
        return -ENOTTY;

    switch (cmd) {
    case 1:                                 /* TTY_RAW            */
        tty_set_canon(false);
        return 0;
    case 2:                                 /* TTY_CANONICAL      */
        tty_set_canon(true);
        return 0;
    default:
        return -EINVAL;
    }
}

/*
 * SYS_mmap(hint, len): private anonymous memory in the process's
 * mmap window (L0 idx 5 -> inherited by fork like any other private
 * mapping). The hint is currently advisory-only -- windows march
 * upward from first use, which is all this OS's binaries need.
 *
 * Frames are allocated BEFORE any table edit so an OOM cannot leave
 * partial mappings behind and unwinding stays trivial.
 */
static long sys_mmap(uint64_t hint, uint64_t len)
{
    struct proc *p = proc_current();
    paddr_t frames[MMAP_PAGES_MAX];
    unsigned npages;
    uint64_t va;
    unsigned i;
    int rc;

    (void)hint;
    if (!p || !p->root_pa)
        return -EINVAL;                     /* kernel threads     */
    if (!len || len > MMAP_PAGES_MAX * PAGE_SIZE)
        return -EINVAL;

    npages = (unsigned)((len + PAGE_SIZE - 1) >> PAGE_SHIFT);

    if (p->mmap_next < USER_MMAP_BASE ||
        p->mmap_next >= USER_SHM_BASE)
        p->mmap_next = USER_MMAP_BASE;
    if ((uint64_t)npages * PAGE_SIZE >
        USER_SHM_BASE - p->mmap_next)
        return -ENOMEM;

    for (i = 0; i < npages; i++) {
        frames[i] = pmm_alloc();

        if (!frames[i]) {
            while (i--)
                pmm_free(frames[i]);
            return -ENOMEM;
        }
        memset((void *)(uintptr_t)frames[i], 0, PAGE_SIZE);
    }

    va = p->mmap_next;
    for (i = 0; i < npages; i++) {
        rc = vmm_map_at(p->root_pa, va + i * PAGE_SIZE, frames[i],
                        VM_READ | VM_WRITE | VM_USER);
        if (rc) {
            /* nothing was published yet -- drop everything       */
            while (i < npages)
                pmm_free(frames[i++]);
            return -ENOMEM;
        }
    }

    p->mmap_next += (uint64_t)npages * PAGE_SIZE;
    return (long)va;
}

static long sys_shmget(uint64_t npages)
{
    return shm_create((unsigned)npages);
}

static long sys_shmat(uint64_t id, uint64_t hint)
{
    return shm_attach((int)id, hint);
}

static long sys_shmdt(uint64_t va)
{
    return shm_detach(va);
}

/* install an open description into the caller's fd table          */
static int fd_install_of(struct proc *p, struct file *f)
{
    if (!p->fds && vfs_proc_fds_init(p))
        return -EMFILE;
    return vfs_fd_install(p->fds, f);   /* consumes our reference  */
}

static long sys_pipe(uint64_t ufds)
{
    struct proc *p = proc_current();
    struct file *rf, *wf;
    int32_t out[2];
    int rfd, wfd;

    if (!p)
        return -EINVAL;

    if (pipe_make(&rf, &wf))
        return -ENFILE;

    rfd = fd_install_of(p, rf);         /* consume refs into table */
    if (rfd < 0) {
        file_close(wf);
        file_close(rf);
        return -EMFILE;
    }
    wfd = fd_install_of(p, wf);
    if (wfd < 0) {
        file_close(wf);
        return -EMFILE;                 /* read end stays installed */
    }

    out[0] = (int32_t)rfd;              /* [0]=read [1]=write      */
    out[1] = (int32_t)wfd;
    if (uacc_copy_out_cur((void *)(uintptr_t)ufds,
                          out, sizeof(out)))
        return -EFAULT;
    return 0;
}

/* dup(fd): same shared open description under a new number        */
static long sys_dup(uint64_t fd)
{
    struct proc *p = proc_current();
    struct file *f;
    int nf;

    if (!p || !p->fds || fd >= PROC_FD_MAX)
        return -EBADF;
    if (!(f = vfs_fd_get(p->fds, (int)fd)))
        return -EBADF;
    nf = vfs_fd_install(p->fds, f);     /* consumes the get() ref  */
    return nf;
}

/*
 * SYS_poll(ufds, nfds, timeout_ms): rescan-after-wake multiplexing.
 * Only READY entries get ->got written back; returns how many.
 */
static long sys_poll(uint64_t ufds, uint64_t nfds, uint64_t timeout_ms)
{
    struct proc *p = proc_current();
    struct uxpollfd kpf[PROC_FD_MAX];
    unsigned want = (unsigned)nfds;
    long ready;
    bool inf = (timeout_ms == (uint64_t)-1);

    if (!p || !ufds || !want)
        return -EINVAL;
    if (want > PROC_FD_MAX)
        want = PROC_FD_MAX;

    for (;;) {
        ready = 0;

        if (uacc_copy_in_cur(kpf, (const void *)(uintptr_t)ufds,
                             sizeof(kpf[0]) * want))
            return -EFAULT;

        for (unsigned i = 0; i < want; i++) {
            struct file *f;
            unsigned m;

            kpf[i].got = 0;
            if (kpf[i].fd < 0)          /* POSIX: ignored entry    */
                continue;
            if (!p->fds ||
                !(f = vfs_fd_get(p->fds, kpf[i].fd))) {
                kpf[i].got = (uint16_t)(POLLERR | POLLNVAL);
                ready++;
                continue;
            }

            m = ipc_file_ready(f);
            m &= (kpf[i].want | POLLERR | POLLHUP);
            file_close(f);              /* drop scan ref           */

            if (m) {
                kpf[i].got = (uint16_t)m;
                ready++;
            }
        }

        if (ready || !timeout_ms)
            break;

        /*
         * Nothing ready yet: sleep on the coarse IPC wake channel
         * (any readiness edge wakes us) or time out.
         */
        ipc_poll_park(inf ? -1 : (int64_t)timeout_ms);
    }

    if (uacc_copy_out_cur((void *)(uintptr_t)ufds, kpf,
                          sizeof(kpf[0]) * want))
        return -EFAULT;
    return ready;
}

/* ---- message queues --------------------------------------------------------- */

static long sys_msgget(uint64_t uname)
{
    struct proc *p = proc_current();
    char name[MQ_NAME_MAX];
    long nl;
    int id, hdl;
    bool created;

    if (!p)
        return -EINVAL;
    nl = uacc_strnlen_user_cur((const void *)(uintptr_t)uname,
                               MQ_NAME_MAX - 1);
    if (nl <= 0)
        return nl < 0 ? -EFAULT : -ENOENT;
    if (uacc_copy_in_cur(name, (const void *)(uintptr_t)uname,
                         (size_t)nl))
        return -EFAULT;
    name[nl] = 0;

    id = mq_open(name, &created);
    if (id < 0)
        return id;

    hdl = mq_handle_install(p, id);
    if (hdl < 0)
        return hdl;
    return hdl;                 /* userspace speaks in handles      */
}

static long sys_msgsnd(uint64_t hdl, uint64_t ubuf, uint64_t len)
{
    struct proc *p = proc_current();
    uint8_t kbuf[MQ_MSG_MAX];
    int id;

    if (!p || len > MQ_MSG_MAX)
        return len > MQ_MSG_MAX ? -EMSGSIZE : -EBADF;
    if ((id = mq_handle_lookup(p, (int)hdl)) < 0)
        return -EBADF;
    if (len && uacc_copy_in_cur(kbuf, (const void *)(uintptr_t)ubuf,
                                (size_t)len))
        return -EFAULT;

    return mq_send_id(id, kbuf, (size_t)len);
}

static long sys_msgrcv(uint64_t hdl, uint64_t ubuf, uint64_t buflen)
{
    struct proc *p = proc_current();
    uint8_t kbuf[MQ_MSG_MAX];
    long got;
    int id;

    if (!p || !buflen || buflen > MQ_MSG_MAX)
        return !buflen ? -EINVAL : -EMSGSIZE;
    if ((id = mq_handle_lookup(p, (int)hdl)) < 0)
        return -EBADF;

    got = mq_receive_id(id, kbuf, (size_t)buflen);
    if (got < 0)
        return got;
    if (uacc_copy_out_cur((void *)(uintptr_t)ubuf, kbuf, (size_t)got))
        return -EFAULT;
    return got;
}

/* ---- unix-domain sockets ------------------------------------------------------ */

static long sys_socketpair(uint64_t ufds)
{
    struct proc *p = proc_current();
    struct file *fa, *fb;
    int32_t out[2];
    int a;

    if (!p)
        return -EINVAL;
    if (usocket_pair(&fa, &fb))
        return -ENFILE;

    a = fd_install_of(p, fa);
    if (a < 0) {
        file_close(fb);
        file_close(fa);
        return -EMFILE;
    }
    {
        int b = fd_install_of(p, fb);

        if (b < 0) {
            file_close(fb);
            return -EMFILE;             /* [0] stays installed      */
        }
        out[0] = (int32_t)a;
        out[1] = (int32_t)b;
    }
    if (uacc_copy_out_cur((void *)(uintptr_t)ufds,
                          out, sizeof(out)))
        return -EFAULT;
    return 0;
}

static long sys_usock_serve(uint64_t upath)
{
    struct proc *p = proc_current();
    char path[USOCK_NAME_MAX];
    struct file *lf;
    long nl;
    int fd;

    if (!p)
        return -EINVAL;
    nl = uacc_strnlen_user_cur((const void *)(uintptr_t)upath,
                               USOCK_NAME_MAX - 1);
    if (nl <= 0)
        return nl < 0 ? -EFAULT : -ENOENT;
    if (uacc_copy_in_cur(path, (const void *)(uintptr_t)upath,
                         (size_t)nl))
        return -EFAULT;
    path[nl] = 0;

    if (usock_serve(path, &lf))
        return -EADDRINUSE;
    fd = fd_install_of(p, lf);
    if (fd < 0) {
        file_close(lf);
        return -EMFILE;
    }
    return fd;
}

static long sys_usock_connect(uint64_t upath)
{
    struct proc *p = proc_current();
    char path[USOCK_NAME_MAX];
    struct file *cf;
    long nl;
    int fd, rc;

    if (!p)
        return -EINVAL;
    nl = uacc_strnlen_user_cur((const void *)(uintptr_t)upath,
                               USOCK_NAME_MAX - 1);
    if (nl <= 0)
        return nl < 0 ? -EFAULT : -ENOENT;
    if (uacc_copy_in_cur(path, (const void *)(uintptr_t)upath,
                         (size_t)nl))
        return -EFAULT;
    path[nl] = 0;

    rc = usock_connect(path, &cf);
    if (rc)
        return rc;
    fd = fd_install_of(p, cf);
    if (fd < 0) {
        file_close(cf);
        return -EMFILE;
    }
    return fd;
}

/* ---- phase 11: AF_INET sockets + select ----------------------------------- */

static long sys_socket(uint64_t type)
{
    return net_sys_socket(type);
}

static long sys_connect(uint64_t fd, uint64_t addr_p, uint64_t len)
{
    if (!proc_current())
        return -EINVAL;
    return net_sys_connect(fd, addr_p, len);
}

static long sys_bind(uint64_t fd, uint64_t addr_p, uint64_t len)
{
    if (!proc_current())
        return -EINVAL;
    return net_sys_bind(fd, addr_p, len);
}

static long sys_listen(uint64_t fd, uint64_t backlog)
{
    return net_sys_listen(fd, backlog);
}

static long sys_accept(uint64_t fd, uint64_t addr_p, uint64_t len_p)
{
    return net_sys_accept(fd, addr_p, len_p);
}

static long sys_send(uint64_t fd, uint64_t ubuf, uint64_t len,
                     uint64_t flags)
{
    if (!proc_current())
        return -EINVAL;
    return net_sys_send(fd, ubuf, len, flags);
}

static long sys_recv(uint64_t fd, uint64_t ubuf, uint64_t len,
                     uint64_t flags)
{
    if (!proc_current())
        return -EINVAL;
    return net_sys_recv(fd, ubuf, len, flags);
}

/*
 * select(nfds, rd_set, wr_set, ex_set, timeout_ms): fd sets are
 * single u64 bitmasks (fds 0..63, far beyond PROC_FD_MAX). Returns
 * the number of ready descriptors; sets are rewritten in place.
 */
static long sys_select(uint64_t nfds, uint64_t rd, uint64_t wr,
                       uint64_t ex, uint64_t timeout_ms)
{
    struct proc *p = proc_current();
    uint64_t rmask = 0, wmask = 0, emask = 0;
    long ready;
    bool inf = (timeout_ms == (uint64_t)-1);

    if (!p || nfds == 0 || nfds > 64u)
        return -EINVAL;

    if (rd && uacc_copy_in_cur(&rmask, (const void *)(uintptr_t)rd, 8))
        return -EFAULT;
    if (wr && uacc_copy_in_cur(&wmask, (const void *)(uintptr_t)wr, 8))
        return -EFAULT;
    if (ex && uacc_copy_in_cur(&emask, (const void *)(uintptr_t)ex, 8))
        return -EFAULT;

    for (;;) {
        ready = 0;
        for (uint64_t b = 0; b < nfds && b < PROC_FD_MAX; b++) {
            struct file *f;
            unsigned m;
            uint64_t bit = 1ull << b;
            bool r_ok = false, w_ok = false, e_ok = false;

            if (!(rmask & bit) && !(wmask & bit) && !(emask & bit))
                continue;
            if (!p->fds || !(f = vfs_fd_get(p->fds, (int)b))) {
                e_ok = true;
            } else {
                m = ipc_file_ready(f);
                file_close(f);
                if (rmask & bit)
                    r_ok = (m & (POLLIN | POLLHUP | POLLERR)) != 0;
                if (wmask & bit)
                    w_ok = (m & (POLLOUT | POLLHUP | POLLERR)) != 0;
                if (emask & bit)
                    e_ok = (m & (POLLERR | POLLHUP)) != 0;
            }

            if (r_ok || w_ok || e_ok) {
                ready++;
                rmask = r_ok ? (rmask | bit) : (rmask & ~bit);
                wmask = w_ok ? (wmask | bit) : (wmask & ~bit);
                emask = e_ok ? (emask | bit) : (emask & ~bit);
            } else {
                if (rmask & bit) rmask &= ~bit;
                if (wmask & bit) wmask &= ~bit;
                if (emask & bit) emask &= ~bit;
            }
        }

        if (ready || !timeout_ms)
            break;

        ipc_poll_park(inf ? -1 : (int64_t)timeout_ms);
    }

    if (rd && uacc_copy_out_cur((void *)(uintptr_t)rd, &rmask, 8))
        return -EFAULT;
    if (wr && uacc_copy_out_cur((void *)(uintptr_t)wr, &wmask, 8))
        return -EFAULT;
    if (ex && uacc_copy_out_cur((void *)(uintptr_t)ex, &emask, 8))
        return -EFAULT;
    return ready;
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
    [SYS_open]       = (sys_fn_t)sys_open,
    [SYS_close]      = (sys_fn_t)sys_close,
    [SYS_lseek]      = (sys_fn_t)sys_lseek,
    [SYS_getdents]   = (sys_fn_t)sys_getdents,
    [SYS_mkdir]      = (sys_fn_t)sys_mkdir,
    [SYS_rmdir]      = (sys_fn_t)sys_rmdir,
    [SYS_unlink]     = (sys_fn_t)sys_unlink,
    /* phase 8: IPC + multiplexing */
    [SYS_ioctl]      = (sys_fn_t)sys_ioctl,
    [SYS_mmap]       = (sys_fn_t)sys_mmap,
    [SYS_shmget]     = (sys_fn_t)sys_shmget,
    [SYS_shmat]      = (sys_fn_t)sys_shmat,
    [SYS_shmdt]      = (sys_fn_t)sys_shmdt,
    [SYS_pipe]       = (sys_fn_t)sys_pipe,
    [SYS_dup]        = (sys_fn_t)sys_dup,
    [SYS_poll]       = (sys_fn_t)sys_poll,
    [SYS_msgget]     = (sys_fn_t)sys_msgget,
    [SYS_msgsnd]     = (sys_fn_t)sys_msgsnd,
    [SYS_msgrcv]     = (sys_fn_t)sys_msgrcv,
    [SYS_socketpair] = (sys_fn_t)sys_socketpair,
    [SYS_usock_serve]   = (sys_fn_t)sys_usock_serve,
    [SYS_usock_connect] = (sys_fn_t)sys_usock_connect,
    /* phase 11: AF_INET sockets + select */
    [SYS_socket]     = (sys_fn_t)sys_socket,
    [SYS_connect]    = (sys_fn_t)sys_connect,
    [SYS_bind]       = (sys_fn_t)sys_bind,
    [SYS_listen]     = (sys_fn_t)sys_listen,
    [SYS_accept]     = (sys_fn_t)sys_accept,
    [SYS_send]       = (sys_fn_t)sys_send,
    [SYS_recv]       = (sys_fn_t)sys_recv,
    [SYS_select]     = (sys_fn_t)sys_select,
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
