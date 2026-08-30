#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "task.h"
#include "vfs.h"

struct file;
struct proc;

/*
 * Inter-process communication core (phase 8, plan items 44+46).
 *
 * Everything lives in static pools sized for this OS's ambitions
 * (a phone, not a server farm) and integrates with the fd world
 * through anonymous vnodes (V_PIPE / V_SOCK -- see include/vfs.h),
 * so read/write/lseek/getattr/poll dispatch through the same
 * vnode_ops vectors regular files use.
 *
 * Poll multiplexing: readiness is reported per-vnode through an
 * optional ->poll op. Every state change in this module (byte moved,
 * endpoint closed, message queued/dequeued, connection accepted...)
 * calls ipc_wake_pollers(), which releases anyone parked inside
 * SYS_poll. Coarse on purpose: rescan-after-wake beats maintaining
 * fine-grained interest sets at this scale, and it is provably
 * correct (no lost edge can survive one extra wake).
 */

/* ---- pipes ------------------------------------------------------------------- */

#define PIPE_BUF_CAP    4096u           /* ring buffer size            */
#define PIPES_MAX          8             /* concurrent pipes            */

/*
 * Kernel-level creation: hands out two open descriptions (the read
 * end O_RDONLY, the write end O_WRONLY) without touching any fd
 * table -- usable from both the syscall layer AND kernel selftests.
 * Each end keeps the pipe alive until closed.
 */
int pipe_make(struct file **rd_out, struct file **wr_out);

/* shared anonymous-vnode factory (pipes and unix sockets use it)  */
struct vnode *ipc_anon_vnode(enum vtype t, const struct vnode_ops *ops,
                             void *priv);

/* state probes used by selftests (no locking side effects) */
bool pipe_readable_bytes_left(const struct file *f, size_t *out);

/* ---- shared memory ------------------------------------------------------------ */

/*
 * Phase 15 raised both limits: UI window surfaces are shm objects
 * now, and one 800x600 XRGB8888 surface is 469 pages (1.83 MiB),
 * with six app windows on top. 512 pages per object = 2 MiB; the
 * static page arrays cost SHM_OBJS_MAX * SHM_PAGE_MAX * 8 bytes
 * (48 KiB) -- noise against the 128 MiB dev image.
 */
#define SHM_PAGE_MAX   512u             /* frames per object           */
#define SHM_OBJS_MAX    12u

/*
 * Mappings live in the dedicated user SHM window -- USER_SHM_BASE,
 * defined in include/proc.h next to the rest of the VA layout (L0
 * index USER_SHM_L0, deliberately outside the fork/teardown range).
 */

/* Create an npages-sized zeroed object; returns id >= 0 or errno.  */
int  shm_create(unsigned npages);

/*
 * Map the object into the CALLING process's address space. va_hint 0
 * picks the next free slot in the SHM window; otherwise the hint must
 * be page-aligned inside the window and unmapped. Returns the VA or a
 * negative errno. Registered in the process's map list so exit cleans
 * up automatically.
 */
long shm_attach(int id, uint64_t va_hint);

/* Detach by VA (undoes exactly one attach). */
int  shm_detach(uint64_t va);

/* Lifecycle probe for selftests: attached-reference count. */
unsigned shm_object_refs(int id);

/* Drop every attach/close owned by `p`; called early in reap_one()
 * BEFORE its page tables are destroyed. Kernel threads pass.       */
void ipc_proc_exit(struct proc *p);

/* Re-map the parent's shm attachments into a fresh child at the same
 * VAs; called from proc_do_fork() once the child's root exists.     */
int  ipc_proc_fork(struct proc *child, struct proc *parent);

/* ---- message queues ------------------------------------------------------------- */

#define MQ_NAME_MAX     24
#define MQ_MSG_MAX      192
#define MQ_DEPTH        12
#define MQ_QUEUES_MAX    8

/* per-process handle/attach budgets live in include/proc.h */

/*
 * Queues are kernel-global named objects holding fixed-size slots.
 * IDs are stable small integers (registry slot numbers); send/
 * receive block when full/empty and integrate with poll wakes.
 * All calls safe from both kernel tasks and processes.
 */
int  mq_open(const char *name, bool *created_out);
int  mq_close_id(struct proc *owner, int id);   /* drop a handle     */

/* descriptor bookkeeping on struct proc (NULL allowed => IDs only) */
int  mq_handle_install(struct proc *p, int id);
int  mq_handle_lookup(struct proc *p, int hdl); /* -> id | -EBADF    */

long mq_send_id(int id, const void *buf, size_t len);
long mq_receive_id(int id, void *buf, size_t buflen);
unsigned mq_pending_count(int id);              /* selftest probe    */

/* ---- poll plumbing ---------------------------------------------------------------- */

/*
 * Readiness bits -- Linux values so userspace headers can match.
 * Returned by vnode ->poll ops and honored by SYS_poll masking.
 */
#define POLLIN      0x001u
#define POLLOUT     0x004u
#define POLLERR     0x008u
#define POLLHUP     0x010u
#define POLLNVAL    0x020u

/* intrinsic readiness of an open description, unfiltered */
unsigned ipc_file_ready(const struct file *f);

/* wake everyone parked in ipc_poll_park(); called on ANY change    */
void ipc_wake_pollers(void);

/*
 * Park the caller until a wake or timeout. timeout_ms == 0 should
 * not be passed here (callers pre-scan); -1 means forever. Returns
 * once RESUMED -- whether something actually became ready is decided
 * by the caller's fresh scan.
 */
void ipc_poll_park(int64_t timeout_ms);

/* subsystem bring-up: clears the registries. */
void ipc_subsys_init(void);

#endif /* IPC_H */