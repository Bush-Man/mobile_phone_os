#ifndef PROC_H
#define PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "exceptions.h"
#include "mm/types.h"
#include "signal.h"
#include "task.h"

/*
 * Process = task + private address space (phase 5, stabilized in
 * phase 8).
 *
 * A process is a struct task whose ->proc field points at one of
 * these. Kernel threads keep proc == NULL and run on the shared
 * kernel TTBR0 (the boot identity map); processes get their own
 * ASID-tagged root table that shares the kernel's identity-mapped
 * subtree at L0 index 0.
 *
 * User virtual layout (48-bit lower half, T0SZ=16, L0 index =
 * va >> 39, one index spans 512 GiB):
 *
 *   idx 0        kernel identity subtree spliced into every root
 *   idx 1        unused
 *   0x0100_0000_0000 ..          program image (idx 2), loader maps
 *                                PT_LOADs upward; brk continues past
 *                                it upward through idx 3/4 space
 *   0x0200_0000_0000             stack top (grows down; the 256 KiB
 *                                reservation stays inside idx 4)
 *   idx 3                        unused spacer between heap room and
 *                                the stack, kept out of the image
 *   0x0280_0000_0000 ..          SYS_mmap private anonymous window
 *                                (idx 5): inherited by fork like any
 *                                other private memory
 *   0x0300_0000_0000 ..          shared-memory attach window (idx 6):
 *                                EXCLUDED from fork copies and from
 *                                address-space teardown -- the shm
 *                                object owns those frames (ipc.c)
 *   < 0x0400_0000_0000           uaccess validity limit
 *
 * The teardown/fork range below ([USER_L0_LO, USER_L0_HI)) covers
 * indices 2..5 inclusive; index 6 must never enter it.
 */
#define USER_L0_LO      2u              /* first L0 index owned by user */
#define USER_MMAP_L0    5u              /* private anon-mmap window    */
#define USER_SHM_L0     6u              /* shared-memory attach window */
#define USER_L0_HI      6u              /* ONE PAST last INHERITED idx */

#define USER_CODE_BASE  0x0000010000000000ULL
#define USER_STACK_TOP  0x0000020000000000ULL
#define USER_MMAP_BASE  0x0000028000000000ULL
#define USER_SHM_BASE   0x0000030000000000ULL
#define USER_STACK_SIZE (256u * 1024u)
#define USER_VA_LIMIT   0x0000040000000000ULL

#define PROC_NAME_MAX   16
#define PROC_KSTACK     (16u * 1024u)   /* dedicated kernel-mode stack  */

/* phase 8 IPC resource budgets owned per process (see include/ipc.h) */
/* PROC_SHM_MAX is 8 for phase 15: the compositor keeps one shm
 * mapping per open app window                                       */
#define PROC_SHM_MAX     8u             /* attached shm regions        */
#define PROC_MQ_MAX      4u             /* open message-queue handles  */

struct proc {
    int          pid;
    char         name[PROC_NAME_MAX];
    struct task *task;                  /* backlink                   */

    /* open files (phase 7); NULL = kernel thread / not yet set up  */
    struct fd_table *fds;

    /* address space */
    paddr_t      root_pa;               /* TTBR0 root (ASID-tagged)   */
    uint8_t      asid;
    uint64_t     asid_gen;              /* allocator generation       */
    uint8_t     *kstack;                /* dedicated EL1 stack        */

    /* lifecycle: parent reaps zombies through waitpid()           */
    bool         alive;                 /* false => zombie            */
    int          exit_code;
    struct proc *parent;
    struct proc *next_all;              /* registry linkage           */

    /* heap top for SYS_brk (page above the highest loaded byte)   */
    vaddr_t      brk;
    /* phase 14: heap floor (image end) -- SYS_brk may shrink down
     * to this but never below, so malloc's arena stays sane       */
    vaddr_t      brk_floor;

    /* phase 8: next free VA for SYS_mmap private mappings          */
    vaddr_t      mmap_next;

    /*
     * Phase 8 IPC state. Shared-memory attach slots use va == 0 as
     * "free" (real VAs always sit in the SHM window far away), and
     * message-queue handles are 1-based with 0 meaning free -- so a
     * zero-filled struct starts fully closed, and fork children
     * inherit nothing here by design.
     */
    struct shm_map {
        uint64_t va;                /* 0 = free                     */
        unsigned npages;
        int      id;
    } shm_maps[PROC_SHM_MAX];
    uint8_t      mq_handles[PROC_MQ_MAX];   /* id+1; 0 = free       */

    /* signals */
    uint32_t     sig_pending;
    bool         in_signal;             /* handler running (no nest)  */
    uint64_t     sigframe_va;           /* frame for SYS_sigreturn    */
    sig_handler_t sig_handler[NSIG];

    /*
     * First user register state: filled by exec/spawn (entry PC,
     * user SP, argc/argv/auxv regs) or by fork (a snapshot of the
     * parent's syscall trap frame with x0 = 0). Consumed once by
     * the process body trampolines.
     */
    struct trap_frame entry_tf;
};

/* ---- subsystem ----------------------------------------------------------- */

void proc_subsys_init(void);            /* boot cpu: config + report     */
void proc_cpu_init(void);               /* per-cpu TCR.A1 + FP untrap    */

struct proc *proc_current(void);        /* current task's proc, or NULL  */

/* scheduler hook: install p's TTBR0+ASID (NULL = shared kernel map) */
void proc_address_space_switch(struct proc *p);

/* zombie + park the CALLING process (signals/faults/exit funnel here) */
void proc_die(struct proc *p, int code, const char *why)
        __attribute__((noreturn));

/* ---- lifecycle ------------------------------------------------------------ */

/*
 * exec-like creation: build a fresh address space from a built-in
 * ELF64 image, place argv/envp/auxv on the user stack and start a
 * new task at EL0. Returns the pid or a negative errno.
 */
int  proc_spawn(const char *img_name,
                const char *const *argv, const char *const *envp);

/* replace the CALLING process (SYS_execve); never returns on success */
long proc_do_exec(const char *name,
                  const char *const *argv, const char *const *envp);

int  proc_do_fork(struct trap_frame *tf);   /* parent ctx: child pid */
void proc_do_exit(int code) __attribute__((noreturn));

/*
 * Blocking reap of a zombie child (want = pid or -1). Returns the
 * child pid and stores its exit code; negative errno otherwise.
 */
int  proc_do_waitpid(int want, int *code_out);

/* non-blocking variant for polling contexts: 1 reaped / 0 pending /
 * -ECHILD if no such child exists */
int  proc_poll_reap(int want, int *code_out, int *pid_out);

int  proc_do_kill(int pid, unsigned sig);
void proc_sigreturn(struct trap_frame *tf);

/* synchronous fault from EL0 (data/instruction abort): kill with report */
void proc_user_fault(struct trap_frame *tf, uint64_t esr, uint64_t far)
        __attribute__((noreturn));

/* pending-signal delivery right before returning to EL0 */
void signal_deliver_pending(struct trap_frame *tf);

/* ---- phase 14: init, orphans, threads ------------------------------------- */

/*
 * Mark the process that owns `pid` as the reaper of orphans (init,
 * PID 1). From then on, every dying process reparents its children
 * to init so their zombies stay reapable -- without this, children
 * of a dead parent would hold a dangling parent pointer forever.
 */
void proc_note_init_pid(int pid);

/* the registered init proc, or NULL before it spawns */
struct proc *proc_init_proc(void);

/* phase 16: registry lookup by pid (zombies included); NULL if
 * unknown -- the release selftest's W^X/ASLR probes use it         */
struct proc *proc_by_pid(int pid);

/* pid of the first live process named `name`, or -1 (selftests)   */
int proc_pid_of_name(const char *name);

/*
 * Phase 14: fill `max` usabi.h psinfo records from the proc registry
 * (zombies included, flagged PSINFO_ZOMBIE). Returns entries written.
 * Backs SYS_psinfo.
 */
struct psinfo_entry;
unsigned proc_psinfo_fill(struct psinfo_entry *ents, unsigned max);

/*
 * Kernel-context reap by pid: used by the phase-14 selftest and the
 * process demo, which run as kernel tasks (no proc => no waitpid).
 * Returns the pid and exit code once the process is a zombie and
 * fully parked, 0 while it is still alive, -ECHILD when no such
 * process exists. Safe to call from task context only.
 */
int proc_kernel_reap(int pid, int *code_out);

/*
 * Blocking wrapper around proc_kernel_reap for kernel tasks: polls
 * until the process is reaped or `timeout_ms` elapses. Returns the pid,
 * -ECHILD if no such process, or -ETIMEDOUT.
 */
int proc_kernel_wait(int pid, int *code_out, uint32_t timeout_ms);

/*
 * Threads within a process (pthread-lite backend). The new thread
 * runs at `pc` on its own user stack top `usp` with x0 = `arg`; its
 * task slot carries a dedicated kmalloc'd EL1 stack. Returns the
 * tid (task slot index) or a negative errno.
 */
int  proc_thread_create(uint64_t pc, uint64_t usp, uint64_t arg);

/* park the CALLING thread's task (leader must use proc_do_exit)   */
void proc_thread_exit(void) __attribute__((noreturn));

/*
 * Reclaim DEAD-and-parked thread slots (frees their kstacks, frees
 * the task slots). Global sweep, safe from any task context: only
 * slots whose scheduler handshake marked them parked are touched,
 * so a thread mid-park is never disturbed. Called from housekeeping
 * and from SYS_thread_exit.
 */
void proc_threads_reclaim(void);

#endif /* PROC_H */
