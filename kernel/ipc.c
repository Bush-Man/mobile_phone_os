/*
 * ipc.c - pipes, shared memory regions, message queues and the poll
 * wakeup plumbing (phase 8).
 *
 * One core spinlock (`ipc_lock`) guards every pool below, following
 * the same discipline the sync layer established:
 *
 *     ipc_lock  ->  task_state_lock      (never the other way round)
 *     nothing blocking ever runs under either
 *
 * Blocking IO endpoints therefore hand-park their caller through
 * ipc_park() with both locks held (no lost-wakeup window), and every
 * releasing site calls ipc_wake() on its endpoint queues plus
 * ipc_wake_pollers() so SYS_poll sleepers rescan the world. That
 * coarse wake-everyone contract is what makes fd multiplexing simple
 * AND correct: any readiness edge produces at least one wake, and a
 * spurious extra wake costs one wasted rescan.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "cpu.h"
#include "chardev.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "ipc.h"
#include "panic.h"
#include "proc.h"
#include "syscall.h"
#include "time.h"
#include "vfs.h"

extern spinlock_t task_state_lock;
uint64_t task_next_key(void);

static spinlock_t ipc_lock = SPINLOCK_INIT;

/* SYS_poll sleepers wait here; ipc_wake_pollers() releases them */
static struct waitqueue poll_wq;

/* pipes currently holding a slot from the static pool */
static bool pipe_used[PIPES_MAX];

static struct {
    uint64_t wakes;                 /* poller wakes issued          */
    uint64_t bytes_flowed;          /* pipe throughput counter      */
} ipcs;

/* ---- core park/wake (mirrors sync.c's two-lock dance) ---------------------- */

static void ipc_park(struct waitqueue *wq)
{
    struct per_cpu *pc = this_cpu();
    daif_state st;

    spin_lock_irqsave(&task_state_lock, &st);
    pc->current->wq_next = wq->head;
    wq->head = pc->current;
    pc->current->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&task_state_lock, st);

    spin_unlock(&ipc_lock);
    sched_park();                   /* never returns                */
}

/* ipc_lock held: release everyone parked on wq */
static void ipc_wake(struct waitqueue *wq)
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

void ipc_wake_pollers(void)
{
    daif_state st;

    ipcs.wakes++;
    spin_lock_irqsave(&task_state_lock, &st);
    while (poll_wq.head) {
        struct task *t = poll_wq.head;

        poll_wq.head = t->wq_next;
        t->wq_next = NULL;
        t->state = TASK_READY;
        t->rq_key = task_next_key();
    }
    spin_unlock_irqrestore(&task_state_lock, st);
}

void ipc_poll_park(int64_t timeout_ms)
{
    struct per_cpu *pc = this_cpu();
    daif_state st;

    /*
     * Parked as TASK_SLEEPING rather than BLOCKED so the timer tick's
     * deadline walker performs our timeout even without a data event;
     * EVENT wakes transition us directly back to READY. An infinite
     * wait carries wake_at = UINT64_MAX, which the (long)(now-that)
     * arithmetic keeps permanently unexpired.
     */
    spin_lock_irqsave(&task_state_lock, &st);
    pc->current->wq_next = poll_wq.head;
    poll_wq.head = pc->current;
    pc->current->state = TASK_SLEEPING;
    pc->current->wake_at =
        timeout_ms < 0 ? UINT64_MAX :
        jiffies_read() + (uint64_t)timeout_ms * TIME_HZ / 1000u + 1u;
    spin_unlock_irqrestore(&task_state_lock, st);

    sched_park();                   /* never returns                */
}

/* ---- anonymous vnode factory ------------------------------------------------- */

struct vnode *ipc_anon_vnode(enum vtype t, const struct vnode_ops *ops,
                             void *priv)
{
    struct vnode *vn = kzalloc(sizeof(*vn));

    if (!vn)
        return NULL;
    vn->ops = ops;
    vn->type = t;
    vn->priv = priv;
    vn->refs = 1;                   /* consumed by file_alloc      */
    return vn;
}

/* ---- pipes ------------------------------------------------------------------- */

struct pipe {
    uint8_t     buf[PIPE_BUF_CAP];
    unsigned    head;               /* producer index               */
    unsigned    len;                /* bytes currently queued       */
    unsigned    readers;
    unsigned    writers;
    bool        rd_dead, wr_dead;   /* latched HUP states for poll  */
    struct waitqueue rq, wq;
};

static const struct vnode_ops pipe_ops;

/* len bytes available at ->head in <=2 contiguous segments        */
static void pipe_copy_out(struct pipe *p, uint8_t *dst, size_t n)
{
    while (n) {
        size_t seg = PIPE_BUF_CAP - p->head;

        if (seg > p->len)
            seg = p->len;
        if (seg > n)
            seg = n;
        memcpy(dst, &p->buf[p->head], seg);
        dst += seg;
        p->head = (p->head + (unsigned)seg) % PIPE_BUF_CAP;
        p->len -= (unsigned)seg;
        n -= seg;
    }
}

static long pipe_read(struct vnode *vn, uint64_t off,
                      void *buf, size_t len)
{
    struct pipe *p = vn->priv;

    (void)off;                      /* pipes have no position       */
    if (!len || !buf)
        return 0;

    for (;;) {
        daif_state s;

        spin_lock_irqsave(&ipc_lock, &s);
        if (p->len) {
            size_t take = p->len < len ? p->len : len;

            pipe_copy_out(p, buf, take);
            ipc_wake(&p->wq);       /* a writer may fit now        */
            spin_unlock_irqrestore(&ipc_lock, s);

            ipcs.bytes_flowed += take;
            ipc_wake_pollers();
            return (long)take;
        }
        if (!p->writers || p->wr_dead) {
            spin_unlock_irqrestore(&ipc_lock, s);
            return 0;               /* EOF                          */
        }
        ipc_park(&p->rq);           /* drops ipc_lock, parks        */
    }
}

/* free space from ->tail as contiguous segments                   */
static void pipe_copy_in(struct pipe *p, const uint8_t *src, size_t n)
{
    while (n) {
        unsigned tail = (p->head + p->len) % PIPE_BUF_CAP;
        size_t seg = PIPE_BUF_CAP - tail;

        if (seg > PIPE_BUF_CAP - p->len)
            seg = PIPE_BUF_CAP - p->len;
        if (seg > n)
            seg = n;
        memcpy(&p->buf[tail], src, seg);
        src += seg;
        p->len += (unsigned)seg;
        n -= seg;
    }
}

static long pipe_write(struct vnode *vn, uint64_t off,
                       const void *buf, size_t len)
{
    struct pipe *p = vn->priv;

    (void)off;
    if (!len)
        return 0;
    if (!buf)
        return -EINVAL;

    for (;;) {
        daif_state s;
        size_t done;

        spin_lock_irqsave(&ipc_lock, &s);

        if (!p->readers || p->rd_dead) {
            spin_unlock_irqrestore(&ipc_lock, s);
            return -EPIPE;          /* broken pipe                  */
        }

        if (p->len < PIPE_BUF_CAP) {
            size_t freecap = PIPE_BUF_CAP - p->len;

            done = freecap < len ? freecap : len;
            pipe_copy_in(p, buf, done);
            ipc_wake(&p->rq);
            spin_unlock_irqrestore(&ipc_lock, s);

            ipcs.bytes_flowed += done;
            ipc_wake_pollers();
            return (long)done;      /* partial write is allowed    */
        }
        ipc_park(&p->wq);           /* full: wait for a reader      */
    }
}


static unsigned pipe_poll(struct vnode *vn)
{
    struct pipe *p = vn->priv;
    unsigned m = 0;
    daif_state s;

    spin_lock_irqsave(&ipc_lock, &s);
    if (p->len)
        m |= POLLIN;
    if (!p->writers || p->wr_dead)
        m |= POLLIN | POLLHUP;      /* EOF is readable "forever"   */
    if (p->len < PIPE_BUF_CAP && p->readers && !p->rd_dead)
        m |= POLLOUT;
    if (!p->readers || p->rd_dead)
        m |= POLLHUP;
    spin_unlock_irqrestore(&ipc_lock, s);
    return m;
}

/*
 * Forward declarations: pipe_destroy must tell the two ends apart,
 * and the tables themselves are defined further down this file.
 */
static const struct vnode_ops pipe_rd_ops;
static const struct vnode_ops pipe_wr_ops;

/* last reference on one end dropped: latch HUP, wake the peer     */
static void pipe_destroy(struct vnode *vn)
{
    struct pipe *p = vn->priv;
    bool was_reader = (vn->ops == &pipe_rd_ops);
    daif_state s;

    spin_lock_irqsave(&ipc_lock, &s);
    if (was_reader) {
        p->rd_dead = true;
        p->readers--;
    } else {
        p->wr_dead = true;
        p->writers--;
    }
    ipc_wake(&p->rq);
    ipc_wake(&p->wq);

    if (!p->readers && !p->writers &&
        p->rd_dead && p->wr_dead && !p->len) {
        spin_unlock_irqrestore(&ipc_lock, s);
        kfree(p);
        ipc_wake_pollers();
        return;
    }
    spin_unlock_irqrestore(&ipc_lock, s);
    ipc_wake_pollers();
}

/*
 * Two ops tables so pipe_destroy() can tell which end died. Pipes
 * ignore lseek/getattr style concerns; only read/write/poll/destroy
 * carry meaning (f_read/f_write enforce accmode via file flags).
 */
static const struct vnode_ops pipe_rd_ops = {
    .read    = pipe_read,
    .write   = pipe_write,          /* write-into-read-end rejected  */
                                   /* by accmode, fn kept for safety*/
    .poll    = pipe_poll,
    .destroy = pipe_destroy,
};

static const struct vnode_ops pipe_wr_ops = {
    .read    = pipe_read,
    .write   = pipe_write,
    .poll    = pipe_poll,
    .destroy = pipe_destroy,
};

int pipe_make(struct file **rd_out, struct file **wr_out)
{
    struct pipe *p;
    struct vnode *rvn, *wvn;
    struct file *rf, *wf;
    unsigned slot = PIPES_MAX;
    daif_state s;

    if (!rd_out || !wr_out)
        return -EINVAL;

    spin_lock_irqsave(&ipc_lock, &s);
    for (unsigned i = 0; i < PIPES_MAX; i++) {
        /* pool occupancy is tracked in a parallel used bitmap     */
        if (!pipe_used[i]) {
            pipe_used[i] = true;
            slot = i;
            break;
        }
    }
    spin_unlock_irqrestore(&ipc_lock, s);
    if (slot == PIPES_MAX)
        return -ENFILE;

    p = kzalloc(sizeof(*p));
    if (!p) {
        spin_lock_irqsave(&ipc_lock, &s);
        pipe_used[slot] = false;
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENOMEM;
    }

    /*
     * Register both end-counts FIRST: every later failure unwinds
     * through close/vn_unref -> pipe_destroy(), which decrements
     * these, so there is exactly one owner-accounting path.
     */
    p->readers = 1;
    p->writers = 1;

    rvn = ipc_anon_vnode(V_PIPE, &pipe_rd_ops, p);
    wvn = ipc_anon_vnode(V_PIPE, &pipe_wr_ops, p);
    rf = rvn ? file_alloc(rvn, O_RDONLY) : NULL;
    wf = wvn ? file_alloc(wvn, O_WRONLY) : NULL;

    if (!rvn || !wvn || !rf || !wf) {
        if (rf)
            file_close(rf);         /* destroys the read end       */
        else if (rvn)
            vn_unref(rvn);          /* never became a file         */
        if (wf)
            file_close(wf);
        else if (wvn)
            vn_unref(wvn);
        /* when both ends went dead above, destroy already freed p */
        spin_lock_irqsave(&ipc_lock, &s);
        pipe_used[slot] = false;
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENOMEM;
    }

    *rd_out = rf;
    *wr_out = wf;
    return 0;
}

bool pipe_readable_bytes_left(const struct file *f, size_t *out)
{
    struct pipe *p;
    daif_state s;

    if (!f || f->vn->type != V_PIPE)
        return false;
    p = f->vn->priv;
    spin_lock_irqsave(&ipc_lock, &s);
    *out = p->len;
    spin_unlock_irqrestore(&ipc_lock, s);
    return true;
}

/* ---- shared memory ------------------------------------------------------------ */

struct shm_obj {
    bool     used;
    unsigned npages;
    paddr_t  pages[SHM_PAGE_MAX];
    unsigned attaches;              /* live process mappings        */
};

static struct shm_obj shm_pool[SHM_OBJS_MAX];
static uint64_t shm_next_off;       /* window allocator (up only)   */

int shm_create(unsigned npages)
{
    int id = -1;
    daif_state s;

    if (!npages || npages > SHM_PAGE_MAX)
        return -EINVAL;

    spin_lock_irqsave(&ipc_lock, &s);
    for (unsigned i = 0; i < SHM_OBJS_MAX; i++)
        if (!shm_pool[i].used) {
            id = (int)i;
            break;
        }

    if (id < 0) {
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENFILE;
    }

    memset(&shm_pool[id], 0, sizeof(shm_pool[id]));
    shm_pool[id].used = true;
    shm_pool[id].npages = npages;
    for (unsigned pg = 0; pg < npages; pg++) {
        paddr_t fr;

        spin_unlock_irqrestore(&ipc_lock, s);
        fr = pmm_alloc();
        if (fr)
            memset((void *)(uintptr_t)fr, 0, PAGE_SIZE);
        spin_lock_irqsave(&ipc_lock, &s);

        if (!fr) {
            /* undo partial allocation */
            for (unsigned k = 0; k < pg; k++) {
                pmm_free(shm_pool[id].pages[k]);
                shm_pool[id].pages[k] = 0;
            }
            shm_pool[id].used = false;
            spin_unlock_irqrestore(&ipc_lock, s);
            return -ENOMEM;
        }
        shm_pool[id].pages[pg] = fr;
    }
    spin_unlock_irqrestore(&ipc_lock, s);
    return id;
}

/*
 * Map/unmap one object's frames over VA [va, va+npages*4K).
 * ipc_lock held; rolls back cleanly on a mapping failure.
 */
static int shm_map_range(struct shm_obj *o, uint64_t va, paddr_t root)
{
    unsigned done = 0;
    int r = 0;

    for (; done < o->npages; done++) {
        r = vmm_map_at(root, va + done * PAGE_SIZE,
                       o->pages[done],
                       VM_READ | VM_WRITE | VM_USER);
        if (r)
            break;
    }
    if (r)
        while (done--)
            vmm_unmap_at(root, va + done * PAGE_SIZE);
    return r;
}

/* drop every mapping for [va, va+npages); frames NOT freed here   */
static void shm_unmap_range(struct shm_obj *o, uint64_t va, paddr_t root)
{
    for (unsigned pg = 0; pg < o->npages; pg++)
        vmm_unmap_at(root, va + pg * PAGE_SIZE);
}

long shm_attach(int id, uint64_t va_hint)
{
    struct proc *p = proc_current();
    struct shm_obj *o;
    uint64_t va;
    int slot = -1;
    long ret;
    daif_state s;

    if (!p || !p->root_pa)
        return -EINVAL;             /* kernel threads cannot attach */
    if (id < 0 || (unsigned)id >= SHM_OBJS_MAX)
        return -EINVAL;

    spin_lock_irqsave(&ipc_lock, &s);

    if (!shm_pool[id].used) {
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENOENT;
    }
    o = &shm_pool[id];

    for (unsigned i = 0; i < PROC_SHM_MAX; i++)
        if (!p->shm_maps[i].va) {
            slot = (int)i;
            break;
        }
    if (slot < 0) {
        spin_unlock_irqrestore(&ipc_lock, s);
        return -EMFILE;
    }

    if (va_hint) {
        uint64_t span = (uint64_t)o->npages * PAGE_SIZE;

        if (!IS_ALIGNED(va_hint, PAGE_SIZE) ||
            va_hint < USER_SHM_BASE ||
            va_hint + span > USER_VA_LIMIT ||
            va_hint + span < va_hint) {
            spin_unlock_irqrestore(&ipc_lock, s);
            return -EINVAL;
        }
        va = va_hint;
    } else {
        va = USER_SHM_BASE + shm_next_off;
        shm_next_off += (uint64_t)o->npages * PAGE_SIZE;
        if (USER_SHM_BASE + shm_next_off >= USER_VA_LIMIT) {
            spin_unlock_irqrestore(&ipc_lock, s);
            return -ENOMEM;         /* window exhausted             */
        }
    }

    if (shm_map_range(o, va, p->root_pa)) {
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENOMEM;
    }

    p->shm_maps[slot].va = va;
    p->shm_maps[slot].npages = o->npages;
    p->shm_maps[slot].id = id;
    o->attaches++;

    ret = (long)va;
    spin_unlock_irqrestore(&ipc_lock, s);
    return ret;
}

/* shared teardown used by shmdt and process exit; ipc_lock held   */
static void shm_do_detach(struct proc *p, unsigned slot)
{
    struct shm_obj *o;
    uint64_t va = p->shm_maps[slot].va;
    int id = p->shm_maps[slot].id;

    if (!va)
        return;

    o = &shm_pool[id];
    shm_unmap_range(o, va, p->root_pa);
    p->shm_maps[slot].va = 0;
    p->shm_maps[slot].npages = 0;
    p->shm_maps[slot].id = 0;

    if (o->attaches)
        o->attaches--;
    if (!o->attaches) {
        for (unsigned pg = 0; pg < SHM_PAGE_MAX; pg++) {
            if (o->pages[pg]) {
                pmm_free(o->pages[pg]);
                o->pages[pg] = 0;
            }
        }
        o->used = false;
        o->npages = 0;
    }
}

int shm_detach(uint64_t va)
{
    struct proc *p = proc_current();
    int found = -ENOENT;
    daif_state s;

    if (!p || !IS_ALIGNED(va, PAGE_SIZE))
        return -EINVAL;

    spin_lock_irqsave(&ipc_lock, &s);
    for (unsigned i = 0; i < PROC_SHM_MAX; i++)
        if (p->shm_maps[i].va == va) {
            shm_do_detach(p, i);
            found = 0;
            break;
        }
    spin_unlock_irqrestore(&ipc_lock, s);
    return found;
}

unsigned shm_object_refs(int id)
{
    unsigned n = 0;
    daif_state s;

    if (id < 0 || (unsigned)id >= SHM_OBJS_MAX)
        return 0;
    spin_lock_irqsave(&ipc_lock, &s);
    n = shm_pool[id].attaches;
    spin_unlock_irqrestore(&ipc_lock, s);
    return n;
}

/* ---- message queues ------------------------------------------------------------- */

struct mqueue {
    bool     used;
    char     name[MQ_NAME_MAX];
    unsigned head;                  /* next slot to drain           */
    unsigned count;
    uint8_t  msgs[MQ_DEPTH][MQ_MSG_MAX];
    uint16_t mlen[MQ_DEPTH];
    unsigned handles;               /* open descriptors             */
    unsigned inflight;              /* senders/receivers inside     */
    struct waitqueue sq, rq;
};

static struct mqueue mq_pool[MQ_QUEUES_MAX];

static void mq_wake_pair(struct mqueue *q)
{
    ipc_wake(&q->sq);
    ipc_wake(&q->rq);
}

int mq_open(const char *name, bool *created_out)
{
    int id = -1, empty = -1;
    daif_state s;

    if (!name || !name[0])
        return -EINVAL;

    spin_lock_irqsave(&ipc_lock, &s);
    for (int i = 0; i < MQ_QUEUES_MAX; i++) {
        if (mq_pool[i].used &&
            !strncmp(mq_pool[i].name, name, MQ_NAME_MAX)) {
            id = i;
            break;
        }
        if (!mq_pool[i].used && empty < 0)
            empty = i;
    }

    if (id >= 0) {
        mq_pool[id].handles++;
        if (created_out)
            *created_out = false;
        spin_unlock_irqrestore(&ipc_lock, s);
        return id;
    }
    if (empty < 0) {
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENFILE;
    }

    memset(&mq_pool[empty], 0, sizeof(mq_pool[empty]));
    mq_pool[empty].used = true;
    kstrlcpy(mq_pool[empty].name, name, MQ_NAME_MAX);
    mq_pool[empty].handles = 1;
    if (created_out)
        *created_out = true;

    spin_unlock_irqrestore(&ipc_lock, s);
    return empty;
}
/*
 * Free a queue once no handles remain and nobody is inside an
 * operation. Waiters always hold ->inflight while parked, so this
 * test is race-free under ipc_lock.
 */
static void mq_try_free(int id)
{
    /* ipc_lock held */
    if (mq_pool[id].used && !mq_pool[id].handles &&
        !mq_pool[id].inflight) {
        mq_pool[id].used = false;
        mq_wake_pair(&mq_pool[id]); /* harmless: wakes stragglers  */
    }
}

int mq_close_id(struct proc *owner, int id)
{
    int r = -ENOENT;
    daif_state s;

    (void)owner;                    /* handles are kernel-global    */
    if (id < 0 || id >= MQ_QUEUES_MAX)
        return -EBADF;

    spin_lock_irqsave(&ipc_lock, &s);
    if (mq_pool[id].used && mq_pool[id].handles) {
        mq_pool[id].handles--;
        r = 0;
        mq_try_free(id);
    }
    spin_unlock_irqrestore(&ipc_lock, s);
    return r;
}

/* descriptor bookkeeping on a struct proc (handles are 1-based;
 * slot value 0 means free, so kzalloc-initialized procs start clean
 * and fork children never inherit queue handles -- deliberate).   */
/* descriptor bookkeeping on a struct proc (handles are 1-based;
 * slot value 0 means free, so kzalloc-initialized procs start clean
 * and fork children never inherit queue handles -- deliberate).   */
static void mq_handle_release(struct proc *p, int hdl)
{
    if (p && hdl > 0 && hdl <= (int)PROC_MQ_MAX)
        p->mq_handles[hdl - 1] = 0;
}

int mq_handle_install(struct proc *p, int id)
{
    if (!p || id < 0 || id >= MQ_QUEUES_MAX)
        return -EINVAL;
    for (unsigned i = 0; i < PROC_MQ_MAX; i++)
        if (p->mq_handles[i] == 0) {
            p->mq_handles[i] = (uint8_t)(id + 1u);
            return (int)(i + 1);
        }
    return -EMFILE;
}

int mq_handle_lookup(struct proc *p, int hdl)
{
    uint8_t v;

    if (!p || hdl <= 0 || hdl > (int)PROC_MQ_MAX)
        return -EBADF;
    v = p->mq_handles[hdl - 1];
    if (!v)
        return -EBADF;
    return (int)v - 1;
}

long mq_send_id(int id, const void *buf, size_t len)
{
    struct mqueue *q;
    daif_state s;

    if (!buf && len)
        return -EINVAL;
    if (len > MQ_MSG_MAX)
        return -EMSGSIZE;

    spin_lock_irqsave(&ipc_lock, &s);
    if (id < 0 || id >= MQ_QUEUES_MAX || !mq_pool[id].used) {
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENOENT;
    }
    q = &mq_pool[id];
    q->inflight++;                  /* object cannot be freed now  */

    for (;;) {
        unsigned tail;

        if (q->count < MQ_DEPTH) {
            tail = (q->head + q->count) % MQ_DEPTH;

            memcpy(q->msgs[tail], buf, len);
            q->mlen[tail] = (uint16_t)len;
            q->count++;
            q->inflight--;

            ipc_wake(&q->rq);
            spin_unlock_irqrestore(&ipc_lock, s);
            ipc_wake_pollers();
            return (long)len;
        }

        ipc_park(&q->sq);           /* full: drop lock + park      */
        /* resumed: queue still valid (our inflight pin)          */
    }
}



long mq_receive_id(int id, void *buf, size_t buflen)
{
    struct mqueue *q;
    daif_state s;

    if (!buf)
        return -EINVAL;

    spin_lock_irqsave(&ipc_lock, &s);
    if (id < 0 || id >= MQ_QUEUES_MAX || !mq_pool[id].used) {
        spin_unlock_irqrestore(&ipc_lock, s);
        return -ENOENT;
    }
    q = &mq_pool[id];
    q->inflight++;

    for (;;) {
        unsigned slot;
        uint16_t mlen;

        if (q->count) {
            slot = q->head;
            mlen = q->mlen[slot];

            memcpy(buf, q->msgs[slot],
                   (size_t)mlen < buflen ? mlen : buflen);
            q->head = (q->head + 1u) % MQ_DEPTH;
            q->count--;
            q->inflight--;

            ipc_wake(&q->sq);
            spin_unlock_irqrestore(&ipc_lock, s);
            ipc_wake_pollers();
            return mlen;            /* full message length         */
        }

        ipc_park(&q->rq);           /* empty: drop lock + park     */
    }
}

unsigned mq_pending_count(int id)
{
    unsigned n = 0;
    daif_state s;

    if (id < 0 || id >= MQ_QUEUES_MAX)
        return 0;
    spin_lock_irqsave(&ipc_lock, &s);
    if (mq_pool[id].used)
        n = mq_pool[id].count;
    spin_unlock_irqrestore(&ipc_lock, s);
    return n;
}

/* ---- readiness + process lifecycle ------------------------------------------------ */

unsigned ipc_file_ready(const struct file *f)
{
    const struct vnode *vn;

    if (!f || !f->vn)
        return POLLERR;
    vn = f->vn;

    if (vn->ops && vn->ops->poll)
        return vn->ops->poll((struct vnode *)vn);

    /*
     * Console chardevs: readability comes from the device's own
     * poll hook (queued tty lines), output is effectively always
     * ready at this scale. Path-backed files/dirs/blocks report
     * both directions so they never stall a poll batch.
     */
    if (vn->type == V_CHARDEV) {
        struct char_dev *cd = vn->priv;

        if (cd && cd->poll)
            return (cd->poll(cd) ? POLLIN : 0u) | POLLOUT;
        return POLLIN | POLLOUT;
    }
    return POLLIN | POLLOUT;
}

/*
 * Release every IPC resource owned by a dying process BEFORE its
 * page tables go away: shared-memory mappings must be removed via
 * their root while it still exists, and queue handles dropped so an
 * unreferenced queue can free itself.
 *
 * Called from reap_one(); kernel threads (no proc) never get here.
 */
void ipc_proc_exit(struct proc *p)
{
    daif_state s;

    if (!p)
        return;

    spin_lock_irqsave(&ipc_lock, &s);
    for (unsigned i = 0; i < PROC_SHM_MAX; i++)
        shm_do_detach(p, i);
    spin_unlock_irqrestore(&ipc_lock, s);

    for (unsigned h = 1; h <= PROC_MQ_MAX; h++) {
        int id = mq_handle_lookup(p, (int)h);

        if (id >= 0) {
            mq_handle_release(p, (int)h);
            mq_close_id(p, id);
        }
    }
}

/* ---- subsystem -------------------------------------------------------------------- */

void ipc_subsys_init(void)
{
    memset(pipe_used, 0, sizeof(pipe_used));
    memset(shm_pool, 0, sizeof(shm_pool));
    memset(mq_pool, 0, sizeof(mq_pool));
    poll_wq.head = NULL;
    shm_next_off = 0;
    memset(&ipcs, 0, sizeof(ipcs));
}

