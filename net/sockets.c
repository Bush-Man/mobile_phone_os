/*
 * sockets.c - AF_INET socket layer (phase 11, item 60).
 *
 * Sockets are anonymous vnodes carrying this ops vector, so poll and
 * the generic fd machinery from phase 8 work unchanged. The fixed
 * slot table holds the protocol pcbs; connect/accept close over the
 * tcp layer directly (blocking calls are fine in syscall context).
 *
 * sockaddr_in ABI (16 bytes, network byte order):
 *   u16 family, u16 port, u32 addr, u8 zero[8]
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "ipc.h"
#include "mm/kheap.h"
#include "net.h"
#include "spinlock.h"
#include "task.h"
#include "uaccess.h"
#include "vfs.h"

extern spinlock_t task_state_lock;
uint64_t task_next_key(void);

#define INET_SOCK_MAX 16u

#define SS_FREE 0u
#define SS_UNCONNECTED 1u
#define SS_CONNECTING 3u
#define SS_LISTENING 4u
#define SS_CONNECTED 5u

struct inet_sock {
    bool        in_use;
    int         type;               /* 1 stream, 2 dgram           */
    uint8_t     state;

    struct tcp_pcb *tp;
    struct udp_pcb *up;

    struct vnode   *vn;
    struct waitqueue wq;            /* accept + data wakeups       */
};

static struct inet_sock isocks[INET_SOCK_MAX];
static spinlock_t iso_lock = SPINLOCK_INIT;

/* ---- sockaddr helpers --------------------------------------------------------- */

struct sockaddr_in_user {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t  zero[8];
} __attribute__((packed));

static void sockaddr_parse(uint64_t user_ptr,
                           uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in_user sa;

    memcpy(&sa, (const void *)(uintptr_t)user_ptr,
           sizeof(sa));   /* syscall layer validated the range        */
    *ip = sa.addr;                  /* user stores BE bytes in u32    */
    *port = ((sa.port & 0xffu) << 8) | (sa.port >> 8);
}

static void sockaddr_fill(uint64_t user_ptr, uint32_t ip, uint16_t port)
{
    struct sockaddr_in_user sa;

    memset(&sa, 0, sizeof(sa));
    sa.family = 2;                  /* AF_INET                     */
    sa.port   = (uint16_t)((port >> 8) | (port << 8));
    sa.addr   = ip;
    memcpy((void *)(uintptr_t)user_ptr, &sa, sizeof(sa));
}

/* ---- wake bridge (net core -> this layer) ------------------------------ */

void net_sock_wake(void *wq)
{
    struct waitqueue *wq2 = wq;
    struct task *cur = current_task();
    daif_state s;
    bool preempt = false;

    spin_lock_irqsave(&task_state_lock, &s);
    while (wq2->head) {
        struct task *t = wq2->head;

        wq2->head = t->wq_next;
        t->wq_next = NULL;
        t->state = TASK_READY;
        t->rq_key = task_next_key();
        if (cur && t->prio < cur->prio)
            preempt = true;
    }
    spin_unlock_irqrestore(&task_state_lock, s);
    if (preempt && this_cpu()->current)
        this_cpu()->need_resched = true;
}

/* ---- vnode ops -------------------------------------------------------------------- */

static unsigned inet_poll(struct vnode *vn)
{
    struct inet_sock *iso = vn->priv;
    unsigned m = 0;

    if (!iso || iso->type != 1)
        return POLLIN | POLLOUT;

    if (!iso->tp)
        return m;

    if (iso->tp->state == TCP_LISTEN)
        return iso->tp->naccepted ? POLLIN : 0u;

    if (tcp_rcv_pending(iso->tp) || tcp_eof(iso->tp))
        m |= POLLIN;
    if (iso->tp->rst_received)
        m |= POLLERR;
    if (tcp_established(iso->tp) && !iso->tp->snd_inflight)
        m |= POLLOUT;
    if (!tcp_established(iso->tp) && iso->tp->state != TCP_SYN_SENT)
        m |= POLLHUP;
    return m;
}

static long inet_read(struct vnode *vn, uint64_t off,
                      void *buf, size_t len)
{
    struct inet_sock *iso = vn->priv;

    (void)off;
    if (!iso)
        return -1;
    if (iso->type == 2)
        return udp_recv(iso->up, buf, len, true);
    if (!iso->tp || iso->state != SS_CONNECTED)
        return -1;
    return tcp_read(iso->tp, buf, len, true);
}

static long inet_write(struct vnode *vn, uint64_t off,
                       const void *buf, size_t len)
{
    struct inet_sock *iso = vn->priv;

    (void)off;
    if (!iso)
        return -1;
    if (iso->type == 2)
        return udp_send(iso->up, buf, len);
    if (!iso->tp || iso->state != SS_CONNECTED)
        return -1;
    return tcp_write(iso->tp, buf, len);
}

static void inet_destroy(struct vnode *vn)
{
    struct inet_sock *iso = vn->priv;

    if (!iso)
        return;
    if (iso->tp) {
        tcp_close(iso->tp, 100u);
        iso->tp = NULL;
    }
    if (iso->up) {
        udp_free(iso->up);
        iso->up = NULL;
    }
    iso->in_use = false;
    iso->vn = NULL;
}

static const struct vnode_ops inet_vops = {
    .read    = inet_read,
    .write   = inet_write,
    .poll    = inet_poll,
    .destroy = inet_destroy,
};

/* ---- slot allocation ------------------------------------------------------------------ */

static struct inet_sock *iso_alloc(int type)
{
    struct inet_sock *iso = NULL;
    struct vnode *vn;
    daif_state s;

    spin_lock_irqsave(&iso_lock, &s);
    for (unsigned i = 0; i < INET_SOCK_MAX; i++)
        if (!isocks[i].in_use) {
            iso = &isocks[i];
            break;
        }
    spin_unlock_irqrestore(&iso_lock, s);
    if (!iso)
        return NULL;

    vn = kzalloc(sizeof(*vn));
    if (!vn)
        return NULL;
    vn->ops  = &inet_vops;
    vn->type = V_SOCK;
    vn->refs = 1;

    memset(iso, 0, sizeof(*iso));
    iso->in_use = true;
    iso->type   = type;
    iso->vn     = vn;
    if (type == 1) {
        iso->tp = tcp_alloc();
        if (!iso->tp) {
            kfree(vn);
            iso->in_use = false;
            return NULL;
        }
        iso->tp->wq = &iso->wq;
    } else if (type == 2) {
        iso->up = udp_alloc();
        if (!iso->up) {
            kfree(vn);
            iso->in_use = false;
            return NULL;
        }
        iso->up->wq = &iso->wq;
    }
    return iso;
}

static struct inet_sock *iso_of_fd(uint64_t fd)
{
    struct vnode *vn = vfs_vnode_of_fd((int)fd);

    if (!vn || vn->type != V_SOCK ||
        vn->ops != &inet_vops)
        return NULL;
    return (struct inet_sock *)vn->priv;
}

/* ---- syscall surface ---------------------------------------------------------------------- */

long net_sys_socket(uint64_t type)
{
    struct inet_sock *iso;
    int is_stream = (type == 1u);

    if (type != 1u && type != 2u)
        return -93;                 /* EPROTONOSUPPORT             */

    iso = iso_alloc(is_stream ? 1 : 2);
    if (!iso)
        return -24;                 /* EMFILE                      */
    iso->state = SS_UNCONNECTED;
    return vfs_install_vnode(iso->vn);
}

long net_sys_connect(uint64_t fd, uint64_t addr_p, uint64_t len)
{
    struct inet_sock *iso = iso_of_fd(fd);
    uint32_t ip;
    uint16_t port;
    int r;

    if (!iso || len < 16u)
        return -107;                /* ENOTCONN? no: EINVAL family */
    sockaddr_parse(addr_p, &ip, &port);

    if (iso->type == 2)
        return udp_connect(iso->up, ip, port);

    iso->state = SS_CONNECTING;
    r = tcp_connect(iso->tp, ip, port, 8000u);
    iso->state = (r == 0) ? SS_CONNECTED : SS_UNCONNECTED;
    return r;
}

long net_sys_bind(uint64_t fd, uint64_t addr_p, uint64_t len)
{
    struct inet_sock *iso = iso_of_fd(fd);
    uint32_t ip;
    uint16_t port;

    if (!iso || len < 16u)
        return -1;
    sockaddr_parse(addr_p, &ip, &port);

    if (iso->type == 2)
        return udp_bind(iso->up, ip, port);
    return tcp_bind(iso->tp, ip, port);
}

long net_sys_listen(uint64_t fd, uint64_t backlog)
{
    struct inet_sock *iso = iso_of_fd(fd);

    (void)backlog;                  /* fixed 4-deep pcb backlog    */
    if (!iso || iso->type != 1)
        return -1;
    iso->state = SS_LISTENING;
    return tcp_listen(iso->tp);
}

long net_sys_accept(uint64_t fd, uint64_t addr_p, uint64_t len_p)
{
    struct inet_sock *iso = iso_of_fd(fd);
    struct tcp_pcb *child;
    struct inet_sock *ciso;
    long nfd;

    if (!iso || iso->type != 1 || iso->state != SS_LISTENING)
        return -1;

    child = tcp_accept(iso->tp, 0u);        /* block indefinitely */
    if (!child)
        return -1;

    ciso = iso_alloc(1);
    if (!ciso) {
        tcp_free(child);
        return -24;
    }
    ciso->tp   = child;
    ciso->state = SS_CONNECTED;
    ciso->tp->wq = &ciso->wq;

    nfd = vfs_install_vnode(ciso->vn);
    if (nfd < 0) {
        tcp_free(child);
        ciso->in_use = false;
        return -24;
    }
    if (addr_p && len_p >= 16u)
        sockaddr_fill(addr_p, child->remote_ip, child->remote_port);
    return nfd;
}

long net_sys_send(uint64_t fd, uint64_t ubuf, uint64_t len,
                  uint64_t flags)
{
    struct inet_sock *iso = iso_of_fd(fd);
    static uint8_t kbuf[1500];
    int r;

    (void)flags;
    if (!iso || len > sizeof(kbuf))
        return len > sizeof(kbuf) ? -90 : -1;

    if (uacc_copy_in_cur(kbuf, (const void *)(uintptr_t)ubuf,
                         len))
        return -14;

    if (iso->type == 2)
        r = udp_send(iso->up, kbuf, len);
    else if (iso->state == SS_CONNECTED && iso->tp)
        r = tcp_write(iso->tp, kbuf, len);
    else
        r = -1;
    return r;
}

long net_sys_recv(uint64_t fd, uint64_t ubuf, uint64_t len,
                  uint64_t flags)
{
    struct inet_sock *iso = iso_of_fd(fd);
    static uint8_t kbuf[1500];
    int r;

    (void)flags;
    if (!iso || !len)
        return -1;
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);

    if (iso->type == 2)
        r = udp_recv(iso->up, kbuf, len, true);
    else if (iso->state == SS_CONNECTED && iso->tp)
        r = tcp_read(iso->tp, kbuf, len, true);
    else
        r = -1;
    if (r <= 0)
        return r;

    if (uacc_copy_out_cur((void *)(uintptr_t)ubuf, kbuf, r))
        return -14;
    return r;
}
