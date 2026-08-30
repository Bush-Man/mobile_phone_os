/*
 * unixsock.c - unix-domain stream sockets (phase 8, plan item 45).
 *
 * Endpoint model: every DATA endpoint owns one inbound ring (bytes
 * it can read). Writing into an endpoint pushes into the PEER's
 * ring, so a pair is just two rings facing each other. Listeners
 * own no data path; they queue connection requests in a bounded
 * backlog and let accept() build the server-side endpoint.
 *
 * Handshake (single-exchange, correct because both halves mutate
 * link state under `unix_lock`):
 *
 *   connect(): build client endpoint -> push {cli} on the listener's
 *     backlog -> park on the CLIENT's handshake wq.
 *   accept(): pop one request -> build server endpoint -> link both
 *     peers together -> wake that client (its connect() now returns).
 *   listener close(): refuses every pended client (wakes it with
 *     -ECONNREFUSED) so nobody can hang forever.
 *
 * Lock ordering matches kernel/ipc.c / kernel/sync.c:
 *   unix_lock -> task_state_lock; nothing blocking underneath.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "cpu.h"
#include "ipc.h"
#include "ipc_ring.h"
#include "irq.h"
#include "lib.h"
#include "unsock.h"
#include "panic.h"
#include "syscall.h"
#include "vfs.h"
#include "vfs.h"

extern spinlock_t task_state_lock;
uint64_t task_next_key(void);

static spinlock_t unix_lock = SPINLOCK_INIT;

enum us_state {
    US_FREE = 0,
    US_DATA,                        /* connected / pairable data ep */
    US_LISTENER,
};

struct us_endpoint {
    enum us_state state;

    /* data endpoints */
    struct ipc_ring ring;
    uint8_t         storage[USOCK_BUF_CAP];
    struct us_endpoint *peer;
    bool            hup;            /* peer side closed             */
    struct waitqueue rq, wq;        /* inbound data / outbound room */

    /* connect() handshake */
    struct waitqueue hs_wq;         /* connector parked here        */
    long             hs_result;     /* filled in by acceptor/refuse */
    int              lslot;         /* listener slot (listeners)    */
    char             name[USOCK_NAME_MAX];  /* listeners only       */

    struct waitqueue accept_wq;     /* parked accept() callers      */
};

struct us_pend {
    struct us_endpoint *cli;        /* NULL = free slot             */
};

static struct us_endpoint ep_pool[USOCK_EP_MAX];
static bool                    ep_used[USOCK_EP_MAX];
static struct us_endpoint     *listeners[USOCK_SERVE_MAX];
static struct us_pend          backlog[USOCK_SERVE_MAX][USOCK_BACKLOG];

/* ---- pool + wake helpers --------------------------------------------------- */

static void us_wake_all(struct waitqueue *wq)
{
    daif_state st;

    spin_lock_irqsave(&task_state_lock, &st);
    while (wq->head) {
        struct task *t = wq->head;

        wq->head = t->wq_next;
        t->wq_next = NULL;
        t->state = TASK_READY;
        t->rq_key = task_next_key();
    }
    spin_unlock_irqrestore(&task_state_lock, st);
}

static void us_park(struct waitqueue *wq)
{
    struct per_cpu *pc = this_cpu();
    daif_state st;

    spin_lock_irqsave(&task_state_lock, &st);
    pc->current->wq_next = wq->head;
    wq->head = pc->current;
    pc->current->state = TASK_BLOCKED;

    spin_unlock(&unix_lock);
    sched_park();                   /* task_state_lock transfers    */
}

void usock_subsys_init(void)
{
    memset(ep_pool, 0, sizeof(ep_pool));
    memset(ep_used, 0, sizeof(ep_used));
    memset(listeners, 0, sizeof(listeners));
    memset(backlog, 0, sizeof(backlog));
}

unsigned usock_file_ready(const struct file *f)
{
    const struct vnode *vn;

    if (!f || !f->vn)
        return POLLERR;
    vn = f->vn;
    if (vn->ops && vn->ops->poll)
        return vn->ops->poll((struct vnode *)vn);
    return POLLIN | POLLOUT;
}


static struct us_endpoint *ep_alloc(void)
{
    struct us_endpoint *e = NULL;
    daif_state s;

    spin_lock_irqsave(&unix_lock, &s);
    for (unsigned i = 0; i < USOCK_EP_MAX; i++)
        if (!ep_used[i]) {
            ep_used[i] = true;
            e = &ep_pool[i];
            break;
        }
    spin_unlock_irqrestore(&unix_lock, s);

    if (e) {
        memset(e, 0, sizeof(*e));
        e->ring.buf = e->storage;
        e->ring.cap = USOCK_BUF_CAP;
        e->lslot = -1;
    }
    return e;
}

/* release after unlinking from every list; ipc side of refs done  */
static void ep_release(struct us_endpoint *e)
{
    daif_state s;

    spin_lock_irqsave(&unix_lock, &s);
    if (!e->peer || e->peer->state == US_FREE) {
        /* peer already gone -> we were the last live half         */
        e->peer = NULL;
    } else {
        e->peer->hup = true;
        us_wake_all(&e->peer->rq);
        us_wake_all(&e->peer->wq);
        e->peer = NULL;
    }
    e->state = US_FREE;
    spin_unlock_irqrestore(&unix_lock, s);
    ipc_wake_pollers();
}

/* ---- vnode ops ---------------------------------------------------------------- */

#include "vfs.h"    /* vnode types for the ops tables below       */

static long us_read(struct vnode *vn, uint64_t off,
                    void *buf, size_t len)
{
    struct us_endpoint *e = vn->priv;

    (void)off;
    if (!len || !buf)
        return 0;

    for (;;) {
        daif_state s;

        spin_lock_irqsave(&unix_lock, &s);
        if (ipc_ring_used(&e->ring)) {
            size_t got = ipc_ring_pull(&e->ring, buf, len);

            us_wake_all(&e->wq);    /* peer writer may have room  */
            spin_unlock_irqrestore(&unix_lock, s);
            ipc_wake_pollers();
            return (long)got;
        }
        if (!e->peer || e->hup) {
            spin_unlock_irqrestore(&unix_lock, s);
            return 0;               /* EOF                         */
        }
        us_park(&e->rq);            /* drops unix_lock + parks     */
    }
}

static long us_write(struct vnode *vn, uint64_t off,
                     const void *buf, size_t len)
{
    struct us_endpoint *e = vn->priv;

    (void)off;
    if (!len)
        return 0;
    if (!buf)
        return -EINVAL;

    for (;;) {
        daif_state s;
        size_t put;

        spin_lock_irqsave(&unix_lock, &s);
        if (!e->peer || e->hup) {
            spin_unlock_irqrestore(&unix_lock, s);
            return -EPIPE;
        }

        put = ipc_ring_push(&e->peer->ring, buf, len);
        if (put) {
            us_wake_all(&e->peer->rq);
            spin_unlock_irqrestore(&unix_lock, s);
            ipc_wake_pollers();
            return (long)put;       /* partial writes allowed     */
        }
        us_park(&e->wq);            /* peer ring full              */
    }
}

static unsigned us_data_poll(struct vnode *vn)
{
    struct us_endpoint *e = vn->priv;
    unsigned m = 0;
    daif_state s;

    spin_lock_irqsave(&unix_lock, &s);
    if (ipc_ring_used(&e->ring))
        m |= POLLIN;
    if (!e->peer || e->hup)
        m |= POLLIN | POLLHUP;
    else if (ipc_ring_free(&e->peer->ring))
        m |= POLLOUT;
    spin_unlock_irqrestore(&unix_lock, s);
    return m;
}

/* destroying the LAST reference of a descriptor's vnode           */
static void us_data_destroy(struct vnode *vn)
{
    ep_release(vn->priv);           /* handles HUP + wakes          */
}

/* ---- listener vnodes -------------------------------------------------------------- */

static long us_listener_read(struct vnode *vn, uint64_t off,
                             void *buf, size_t len)
{
    (void)vn; (void)off; (void)buf; (void)len;
    return -EINVAL;                 /* listeners carry no data      */
}

static long us_listener_write(struct vnode *vn, uint64_t off,
                              const void *buf, size_t len)
{
    (void)vn; (void)off; (void)buf; (void)len;
    return -ENOTCONN;
}

static unsigned us_listener_poll(struct vnode *vn)
{
    struct us_endpoint *e = vn->priv;
    unsigned m = POLLIN | POLLOUT;  /* a listener always accepts... */
    daif_state s;

    spin_lock_irqsave(&unix_lock, &s);
    if (e->state != US_LISTENER)
        m |= POLLERR;               /* raced with close             */
    spin_unlock_irqrestore(&unix_lock, s);
    return m;
}

static void us_listener_destroy(struct vnode *vn)
{
    struct us_endpoint *e = vn->priv;
    daif_state s;

    spin_lock_irqsave(&unix_lock, &s);

    if (e->lslot >= 0 && e->lslot < (int)USOCK_SERVE_MAX &&
        listeners[e->lslot] == e)
        listeners[e->lslot] = NULL;

    /* refuse every queued connector                                */
    for (unsigned b = 0; b < USOCK_BACKLOG; b++) {
        struct us_pend *p = &backlog[e->lslot][b];
        struct us_endpoint *cli = p->cli;

        if (cli) {
            p->cli = NULL;
            cli->hs_result = -ECONNREFUSED;
            us_wake_all(&cli->hs_wq);
        }
    }

    e->state = US_FREE;
    spin_unlock_irqrestore(&unix_lock, s);
    ipc_wake_pollers();
}



/* ---- public API ---------------------------------------------------------------- */

static const struct vnode_ops us_data_ops = {
    .read    = us_read,
    .write   = us_write,
    .poll    = us_data_poll,
    .destroy = us_data_destroy,
};

static const struct vnode_ops us_listener_ops = {
    .read    = us_listener_read,
    .write   = us_listener_write,
    .poll    = us_listener_poll,
    .destroy = us_listener_destroy,
};

static struct file *endpoint_file(struct us_endpoint *e, bool listener)
{
    struct vnode *vn =
        ipc_anon_vnode(V_SOCK,
                       listener ? &us_listener_ops : &us_data_ops,
                       e);
    struct file *f;

    if (!vn)
        return NULL;
    f = file_alloc(vn, O_RDWR);
    if (!f)
        vn_unref(vn);               /* destroy hook handles state  */
    return f;
}

int usocket_pair(struct file **a_out, struct file **b_out)
{
    struct us_endpoint *a, *b;
    struct file *fa, *fb;

    if (!a_out || !b_out)
        return -EINVAL;

    a = ep_alloc();
    if (!a)
        return -ENOMEM;
    b = ep_alloc();
    if (!b) {
        ep_release(a);
        return -ENOMEM;
    }

    /* link first: after this the pair is live and every close
     * participates in mutual HUP bookkeeping                       */
    spin_lock(&unix_lock);
    a->state = US_DATA;
    b->state = US_DATA;
    a->peer = b;
    b->peer = a;
    spin_unlock(&unix_lock);

    fa = endpoint_file(a, false);
    fb = endpoint_file(b, false);
    if (!fa || !fb) {
        if (fa)
            file_close(fa);         /* drops refs -> destroys      */
        else
            ep_release(a);
        if (fb)
            file_close(fb);
        else
            ep_release(b);
        return -ENOMEM;
    }

    *a_out = fa;
    *b_out = fb;
    return 0;
}

/* distinguishes EADDRINUSE vs ENFILE for the failed-publish path  */
/* distinguishes EADDRINUSE vs ENFILE for the failed-publish path  */
static bool listeners_lookup_full(const char *path);

int usock_serve(const char *path, struct file **listen_out)
{
    struct us_endpoint *l;
    unsigned slot = USOCK_SERVE_MAX;
    daif_state s;

    if (!path || !path[0] || !listen_out)
        return -EINVAL;
    if (strlen(path) >= USOCK_NAME_MAX)
        return -ENAMETOOLONG;

    l = ep_alloc();
    if (!l)
        return -ENOMEM;

    /* build the descriptor before publishing so an OOM cannot
     * leave a half-registered listener behind                      */
    {
        struct file *f =
            endpoint_file(l, true);

        if (!f) {
            ep_release(l);
            return -ENOMEM;
        }

        spin_lock_irqsave(&unix_lock, &s);

        for (unsigned i = 0; i < USOCK_SERVE_MAX; i++) {
            if (listeners[i] &&
                !strncmp(listeners[i]->name, path, USOCK_NAME_MAX)) {
                slot = USOCK_SERVE_MAX;
                break;
            }
            if (!listeners[i] && slot == USOCK_SERVE_MAX)
                slot = i;
        }
        if (slot == USOCK_SERVE_MAX) {
            spin_unlock_irqrestore(&unix_lock, s);
            file_close(f);          /* destroy reclaims endpoint  */
            return listeners_lookup_full(path) ? -EADDRINUSE : -ENFILE;
        }

        l->state = US_LISTENER;
        l->lslot = (int)slot;
        kstrlcpy(l->name, path, USOCK_NAME_MAX);
        listeners[slot] = l;

        spin_unlock_irqrestore(&unix_lock, s);
        *listen_out = f;
    }
    return 0;
}

/* distinguishes EADDRINUSE vs ENFILE for the failed-publish path  */
static bool listeners_lookup_full(const char *path)
{
    daif_state s;

    spin_lock_irqsave(&unix_lock, &s);
    for (unsigned i = 0; i < USOCK_SERVE_MAX; i++)
        if (listeners[i] &&
            !strncmp(listeners[i]->name, path, USOCK_NAME_MAX)) {
            spin_unlock_irqrestore(&unix_lock, s);
            return true;
        }
    spin_unlock_irqrestore(&unix_lock, s);
    return false;
}



/* ---- accept / connect handshake ---------------------------------------------- */

long usock_accept(struct file *listener, struct file **conn_out)
{
    struct us_endpoint *l;
    struct us_endpoint *cli = NULL;
    struct us_endpoint *srv;
    struct file *f;
    daif_state s;

    if (!listener || !conn_out || listener->vn->type != V_SOCK)
        return -ENOTSOCK;
    l = listener->vn->priv;

    for (;;) {
        spin_lock_irqsave(&unix_lock, &s);

        if (l->state != US_LISTENER) {
            spin_unlock_irqrestore(&unix_lock, s);
            return -EINVAL;         /* listener closed meanwhile   */
        }

        /* pop the oldest queued client                             */
        for (unsigned b = 0; b < USOCK_BACKLOG && !cli; b++) {
            struct us_pend *p = &backlog[l->lslot][b];

            if (p->cli) {
                cli = p->cli;
                p->cli = NULL;
            }
        }

        if (!cli) {
            us_park(&l->accept_wq); /* drop lock + park            */
            continue;               /* revalidate on resume        */
        }
        break;                      /* holding cli, lock still held*/
    }
    spin_unlock_irqrestore(&unix_lock, s);

    /*
     * From here the client is ours alone (nobody else saw the
     * backlog slot), so the slow allocator calls stay outside the
     * spinlock exactly like everywhere else in this subsystem.
     * The client keeps sleeping until its handshake completes.
     */
    srv = ep_alloc();
    if (!srv) {
        spin_lock_irqsave(&unix_lock, &s);
        cli->hs_result = -ENOMEM;
        us_wake_all(&cli->hs_wq);
        spin_unlock_irqrestore(&unix_lock, s);
        ipc_wake_pollers();
        return -ENOMEM;
    }
    srv->state = US_DATA;

    spin_lock_irqsave(&unix_lock, &s);
    srv->peer = cli;
    cli->peer = srv;
    cli->hs_result = 0;
    us_wake_all(&cli->hs_wq);       /* connect() unblocks           */
    spin_unlock_irqrestore(&unix_lock, s);

    f = endpoint_file(srv, false);
    if (!f) {
        ep_release(srv);            /* tears down both halves      */
        ep_release(cli);
        return -ENOMEM;
    }
    ipc_wake_pollers();
    *conn_out = f;
    return 0;
}


long usock_connect(const char *path, struct file **conn_out)
{
    struct us_endpoint *cli;
    bool queued = false;
    long r;

    if (!path || !path[0] || !conn_out)
        return -EINVAL;

    cli = ep_alloc();
    if (!cli)
        return -ENOMEM;

    {
        daif_state s;

        spin_lock_irqsave(&unix_lock, &s);

        for (unsigned i = 0; i < USOCK_SERVE_MAX && !queued; i++) {
            struct us_endpoint *L = listeners[i];

            if (!L || L->state != US_LISTENER ||
                strncmp(L->name, path, USOCK_NAME_MAX))
                continue;

            /* queue into the listener's backlog                    */
            for (unsigned b = 0; b < USOCK_BACKLOG; b++)
                if (!backlog[i][b].cli) {
                    backlog[i][b].cli = cli;
                    queued = true;
                    break;
                }
        }

        if (!queued) {
            bool live = listeners_lookup_full(path);

            spin_unlock_irqrestore(&unix_lock, s);
            ep_release(cli);
            return live ? -ECONNREFUSED : -ENOENT;
        }

        cli->state = US_DATA;
        cli->hs_result = -ECONNREFUSED;     /* default outcome     */
        spin_unlock_irqrestore(&unix_lock, s);
    }

    /*
     * Wait for an acceptor to link us or a refuser to cancel.
     * We re-check everything under unix_lock every time we are
     * woken, so no edge can be lost between "linked" and "wake".
     */
    {
        daif_state s;

        for (;;) {
            spin_lock_irqsave(&unix_lock, &s);

            if (cli->peer) {
                r = 0;              /* accepted                     */
                break;
            }
            if (cli->hs_result < 0) {
                r = cli->hs_result; /* refused / listener gone      */
                break;
            }

            /* us_park drops unix_lock itself: releasing it here too
             * would reopen the window where an acceptor's wake lands
             * before we are on the queue, and lose it */
            us_park(&cli->hs_wq);
        }
        spin_unlock_irqrestore(&unix_lock, s);
    }

    if (r == 0) {
        struct file *f = endpoint_file(cli, false);

        if (!f) {
            ep_release(cli);
            return -ENOMEM;
        }
        *conn_out = f;
    } else {
        ep_release(cli);
    }
    return r;
}


