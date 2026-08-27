# Phase 8 — IPC, Sync & POSIX-ish API (Implementation Log)

Milestone scope reached: sleeping mutexes and semaphores with owner
tracking and a cycle-detecting deadlock detector (kernel/sync.c);
anonymous pipes over V_PIPE vnodes, refcounted mappable shared-memory
regions, fixed-slot message queues and the SYS_poll multiplexer
(kernel/ipc.c + include/ipc_ring.h); unix-domain stream sockets with
socketpair plus a named serve/connect/accept transport with backlog
(kernel/unixsock.c); the stabilized syscall surface (ioctl/mmap/
shmget/shmat/shmdt/pipe/dup/poll/msgget/msgsnd/msgrcv/socketpair/
unix-serve/unix-connect added to open/read/write/close/lseek/fork/
exec/wait/exit/kill/sleep). Per the standing coordination decision,
final integration builds/QEMU smoke runs are deferred to the
project-complete point; every new/edited translation unit was kept
compile-clean via per-file `aarch64-linux-gnu-gcc -fsyntax-only
-Wall -Wextra -ffreestanding -fno-builtin -mgeneral-regs-only`
checks after each batch (zero warnings/errors at commit time for all
phase-8 files; the two pre-existing phase-5 items listed in
docs/PHASE_7.md -- proc_user_fault %llx cast and execve uaccess
casts -- were both FIXED this phase).

## Order of files written

### Batch A — sync core (plan item 43)

1. **`include/task.h` / `kernel/task.c`** — one new field:
   `struct task::lock_wait`, "the mutex I intend to sleep on".
   Written/cleared under the sync core's lock; NULL while running or
   parked anywhere else. This single field is what lets the detector
   walk ownership chains without racing intent updates.
2. **`include/sync.h`** — data types first: `struct kmutex` (name,
   owner task pointer, waitqueue), `struct ksem` (name, count, max,
   waitqueue), `struct sync_stats`, deadlk/ebusy result constants;
   APIs kmutex_init/lock/try/unlock/owned_by_current +
   kmutex_first_waiter (debug/selftest surface, added in batch F)
   and ksem_init/wait/trywait/post.
3. **`kernel/sync.c`** — one core spinlock (`sync_lock`) guarding all
   primitive state and stats; lock order strictly `sync_lock ->
   task_state_lock`, nothing blocking underneath. Parking is done by
   hand rather than through wait_sleep_when(): check/enqueue/release
   transitions happen under BOTH locks so release+wake is atomic
   against enqueue -- no lost-wakeup window by construction.
   `wake_batch()` mirrors wait_wake_all's scheduler bookkeeping
   (READY state, FIFO key, priority-preempt flag, wakeup counter).
   The detector walks mutex->owner->lock_wait->... chains under the
   core lock with a bounded visited-set (16 > any possible acyclic
   chain here) and returns -EDEADLK instead of hanging; semaphore
   waits deliberately contribute no edges (untracked ownership --
   documented design limit).
4. **Batch A fixup**: format-string arity bug caught by the
   syntax-only sweep before it could become a runtime garble.

Commit: "Add blocking mutexes and semaphores with owner tracking and
deadlock detection (phase 8)".

### Batch B — paying off the documented phase-7 debt

5. **`fs/fat32.c`** — deleted the hand-rolled flag+waitqueue
   big-fs-lock (`struct fat_sem`, bfl_busy predicate,
   wait_sleep_when loop) that phase 7 duplicated because no real
   sleeping mutex existed yet; kept the `sem_acquire/sem_release`
   call-site names as thin wrappers over kmutex_lock/kmutex_unlock
   (panic instead of hang if the detector ever fires around an fs
   lock). ~24 call sites untouched -- only the primitive beneath
   them changed.
6. **`fs/ext2.c`** — identical surgery for `struct e2_sem`.

Commit: "Replace ad-hoc sleeping fs locks in fat32/ext2 with real
mutexes from the phase-8 sync core".

### Batch C — IPC core: pipes, shm, message queues, poll plumbing (44+46)

7. **`include/vfs.h`** — enum vtype grew V_PIPE/V_SOCK (anonymous
   nodes that never appear via path resolution);
   `struct vnode_ops::poll` optional readiness hook returning
   POLLIN/POLLOUT/POLLHUP/POLLERR bits without blocking.
8. **`include/ipc.h`** — pipe/shm/mqueue/poll layer contract: static
   pool limits (PIPES_MAX 8, SHM_PAGE_MAX 16 frames/object,
   MQ_QUEUES_MAX 8 x 12 slots x 192 bytes), pipe_make handing out two
   O_RDONLY/O_WRONLY descriptions without touching fd tables,
   shm_create/attach/detach/object_refs, mq_open/send/receive-by-id
   plus the per-process handle helpers, ipc_file_ready /
   ipc_wake_pollers / ipc_poll_park, ipc_proc_exit lifecycle hook.
9. **`include/syscall.h`** — errno additions used across the phase:
   ENFILE/EPIPE/EMSGSIZE/ENOBUFS/EADDRINUSE/ENOTCONN/ENOTSOCK/
   ECONNREFUSED.
10. **`kernel/ipc.c`** — same two-lock parking discipline as sync.c
    (ipc_lock -> task_state_lock; sched_park after enqueueing under
    both). Pipes: ring with <=2-segment copies, readers/writers
    refcounts, HUP latching through TWO ops tables (pipe_rd_ops vs
    pipe_wr_ops) so destroy() knows which end died; writes return
    partial counts like POSIX byte streams, EOF as 0 reads, EPIPE
    into vanished readers. SYS_poll sleepers park as TASK_SLEEPING
    on their own queue with wake_at = deadline (or UINT64_MAX for
    infinity) so the existing timer-tick deadline walker performs
    timeouts with zero scheduler changes -- every IPC state change
    calls ipc_wake_pollers() which releases the whole lot
    (rescan-after-wake beats maintaining per-fd interest sets at
    this scale; correctness costs one extra rescan). Shared memory:
    frames allocated up front (partial-create unwinds), mapped into
    the CALLING process root via vmm_map_at
    (VM_READ|VM_WRITE|VM_USER) inside the dedicated USER_SHM window,
    attach slots recorded on struct proc (va==0 = free) and released
    BOTH by explicit shmdt AND automatic teardown at reap time --
    frames are freed exactly once, when the LAST attach drops, not
    when any single process dies.
11. **`fs/vfs.c`** — f_lseek rejects V_PIPE/V_SOCK with -ESPIPE.

Commit: "Add IPC core: pipes over anonymous vnodes, refcounted shared
memory, message queues and poll wakeup plumbing (phase 8)".

### Batch D — unix-domain sockets (45)

12. **`include/ipc_ring.h`** — tiny SPSC byte ring (pull/push with
    bounded memcpy progress) factored out BEFORE unixsock.c needed
    its own buffers; pipes keep their inline copy loops.
13. **`include/unsock.h` / `kernel/unixsock.c`** — endpoint model:
    every DATA endpoint owns one inbound ring ("writing pushes into
    the PEER'S ring"), listeners own none and carry a bounded
    backlog (4) of connection requests. Single-exchange handshake,
    correct because both halves mutate link state under `unix_lock`:
    connect() builds the client endpoint, queues itself, parks on
    its OWN hs_wq; accept() pops a request, links server<->client,
    wakes the connector; listener close refuses everyone pended with
    -ECONNREFUSED so nothing can hang forever. connect answers
    -ENOENT (never served) vs -ECONNREFUSED (listener died or
    backlog full) distinctly. A lock self-deadlock in the first
    accept() draft (ep_alloc taking unix_lock while already held)
    was caught by review and restructured before commit.
14. **`include/ipc.h` / `kernel/ipc.c`** — exported the shared
    anonymous-vnode factory (ipc_anon_vnode) so sockets build their
    endpoints with the exact same vnode/refs machinery pipes use.

Commit: "Add unix-domain stream sockets: socket pairs and named
local transport with backlog (phase 8)".

### Batch E — stabilize processes/syscalls (47)

15. **`include/proc.h`** — corrected user VA layout documentation and
    the L0 index math (see "bugs found" below): USER_L0_LO=2,
    USER_L0_HI=6 covers indices 2..5 [program, brk room, stack(4),
    mmap window(5)]; idx 6 is the new SHM window and must NEVER be
    copied at fork nor torn down per-process (the object owns those
    frames). Added struct fields: vaddr_t mmap_next (SYS_mmap window
    allocator), shm_maps[PROC_SHM_MAX], mq_handles[PROC_MQ_MAX]
    (1-based; zero-filled = closed, so fork children inherit neither
    by design).
16. **`kernel/proc.c`** — proc_spawn records parent = caller's proc
    (previously left NULL => children were UNREAPABLE even in-tree);
    proc_do_waitpid returns -ESRCH for kernel threads instead of
    panicking (they have no children; only reaping semantics
    changed); reap_one() calls ipc_proc_exit(zombie) BEFORE
    space_destroy because shmdt-at-exit needs the root table alive;
    mmap_next initialized at spawn and inherited at fork; execve
    uaccess casts (documented phase-5 debt) fixed properly;
    mark_zombie/reap sequencing notes now document WHY task-slot
    reuse is safe.
17. **`include/syscall.h`** — numbers 20..33 appended: ioctl/mmap/
    shmget/shmat/shmdt/pipe/dup/poll/msgget/msgsnd/msgrcv/
    socketpair/usock_serve/usock_connect.
18. **`include/vfs.h`** — compact poll ABI next to the getdents
    record layout: `struct uxpollfd {i32 fd; u16 want; u16 got}`
    packed; SYS_poll fills got ONLY for ready entries.
19. **`kernel/syscall.c`** — handlers wired into the dispatch table:
    ioctl validates fd->vnode type/name against the console chardev
    and flips tty line discipline (raw/canonical); mmap hands out
    pages from the private idx-5 window (frames allocated BEFORE any
    table edit so unwind is trivially leak-free); pipe installs the
    pair and copies fds back through uaccess; dup re-installs the
    SAME shared description (refs do the aliasing); poll scans under
    fd refs, reports HUP/ERR regardless of want, parks on the coarse
    wake channel between scans, honours timeout==0 and -1; msg*
    translate handles to ids then run the blocking core calls;
    socketpair/serve/connect install endpoint descriptors like any
    file. SYS_uptime_ms had been accidentally consumed mid-edit and
    was restored within the same batch.

Commits: "Stabilize process/syscall surface ..." + "Add phase-8
syscall surface ...".

### Batch F — verification, demo & bring-up

20. **`kernel/selftest_ipc.c`** — "ipctest" battery, ordered: mutex
    try/owner/unlock, self re-lock detected (-EDEADLK without
    parking), CROSS-TASK A<->B cycle where main waits until the
    helper's intent edge is PUBLISHED on B's waiter queue (via
    kmutex_first_waiter) before closing the ring from its side --
    that detail prevents racing into a REAL deadlock instead of a
    detection event; semaphore ping-pong across tasks; pipe
    create/write/read/poll flags/EPIPE-after-reader-death; shm
    create/attach/refcount/detach; mqueue open/reopen-same-slot/
    FIFO send/receive by direct ID (the handle layer is exercised
    by the user demo since kernel tasks carry no handle table);
    socketpair bidirectional traffic then named transport with an
    echo-server TASK doing the concurrent accept(). Summary line
    "selftest: ipc ok".
21. **`userspace/ipcdemo.c` / ipcdemo.ld / builtin_imgs.S /
    Makefile** — milestone program at EL0: parent mmaps a private
    page (mmap-inheritance proof), shmget+shmat one page and plants
    0x600DF00D, pipe(), fork(); child verifies the inherited
    mapping, latches 0xCAFEF00D into slot[1], sends two fixed lines
    over the pipe; parent checks pipe round-trip AND shared-memory
    flip, waitpid-reaps exit code 7, exits 21. Embedded verbatim as
    the second built-in image next to hello.
22. **`kernel/phase8.c` / `kernel/main.c` / Makefile harness arg** —
    boot-time registry init (ipc_subsys_init + usock_subsys_init),
    spawns ipctest + ipcdemo starter after fstest; banner bumped to
    `phase 8` and `make test` passes 8 to the cumulative-phase
    serial harness.

## Bugs found (and fixed) along the way

- **USER_L0_LO/HI never covered the real mappings.** Phase 5 set
  [8,16) while T0SZ=16 puts the program at L0 idx 2 and the stack at
  idx 4 -- so vmm_copy_space/vmm_root_release walked EMPTY indices on
  every fork and exit: children silently lost inherited memory
  content and dying processes leaked their user frames forever.
  Fixed in item 15; phases were compile-checked only by policy,
  which is exactly why this survived until an integration pass.
- **Kernel-thread callers of proc_do_waitpid would panic** through
  the procdemo chain in kernel/main.c -- now -ESRCH.
- **spawn'd children were unreapable**: spawn never assigned
  ->parent, so waitpid(-1) scanned for children of whichever proc
  happened to call it. Fixed by recording the spawner.
- **SYS_uptime_ms clobbered** during syscall.c edits and restored
  before anything shipped (caught by the per-file sweep).

## Design decisions worth remembering

- **Two-lock hand-park everywhere.** sync/ipc/unsock each hold their
  subsystem spinlock while enqueueing the caller onto a waitqueue
  and dropping into sched_park(). Wakers move state + drain queues
  under the SAME ordering (subsystem -> task_state). Nothing blocks
  under either lock; no primitive uses the drop-check-recheck
  pattern anymore, eliminating that lost-wakeup window class.
- **Anonymous vnodes, not a second namespace.** Pipes/sockets ride
  existing vnode ops vectors and fd tables -- read/write/poll on
  them worked BEFORE their creation syscalls existed.
- **Coarse poll waking.** Any readiness edge wakes ALL SYS_poll
  sleepers; they rescan. Interest sets would shave microseconds at
  correctness risk we don't need yet.
- **SHM lifetime lives with the object**, not processes: attach
  maps + refcounts, reap unmaps but never frees, last-detach frees.
- **fat/ext2 keep their old wrapper names** for a minimal diff;
  behaviour identical except debuggability.

## Verification status

Per coordination decision (builds/QEMU smoke runs deferred until the
project-complete point), no `make` target was run this phase. Every
phase-8 translation unit was checked with
`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Wextra -ffreestanding
-fno-builtin -mgeneral-regs-only` after each batch:

```
CLEAN include/{sync.h,ipc.h,ipc_ring.h,unsock.h,proc.h,vfs.h,syscall.h}
CLEAN kernel/{sync.c,ipc.c,unixsock.c,selftest_ipc.c,phase8.c,main.c,task.c}
CLEAN kernel/syscall.c kernel/proc.c*
CLEAN fs/{vfs.c,fat32.c,ext2.c} userspace/ipcdemo.c
(*proc.c retains ONE pre-existing warning: the phase-5 proc_user_fault
 %llx cast -- present at HEAD throughout earlier phase logs as well.)
```

When integration lands expect, in order:

```
make test          # harness passes phase 8; cumulative criteria:
                   #   banner/uptime/rx echo + ping/pong rounds
                   #   hello EL0 chain exiting 42
serial output adds:
   ipc: registries online
   ipctest: ... battery lines ...
   selftest: ipc ok
   [demo] ipcdemo spawned pid N
   ipcdemo-child: pid M inherited-mmap ok
   ipcdemo: PIPE round-trip ok
   ipcdemo: SHM cross-process ok
   ipcdemo: reaped child code 7
   ipcdemo: exiting 21
```

Notes carried forward:

- Semaphores stay invisible to the deadlock detector (untracked
  ownership); mutex chains are the supported graph.
- No fcntl/close-on-exec, no SIGCHLD-on-exit delivery, no EINTR for
  blocked readers -- unchanged standing debts from phases 5-7.
- mq handles are per-process tokens over kernel-global queues; FIFO
  order guaranteed, message size capped at 192 bytes.
- SYS_mmap ignores hint/prot beyond range checks (RW-anonymous
  only); W^X refinements belong to phase 16 hardening.
- usock listeners exist only while someone holds their descriptor;
  there is deliberately no filesystem-visible unix node until phase
  14 needs one for daemon namespaces.

