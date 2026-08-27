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
 * Process = task + private address space (phase 5).
 *
 * A process is a struct task whose ->proc field points at one of
 * these. Kernel threads keep proc == NULL and run on the shared
 * kernel TTBR0 (the boot identity map); processes get their own
 * ASID-tagged root table that shares the kernel's identity-mapped
 * subtree at L0 index 0.
 *
 * User virtual layout (48-bit lower half, T0SZ=16):
 *
 *   0x0000_0000_0000 .. <512 GiB   kernel identity subtree (EL1 only)
 *   0x0100_0000_0000               program base (L0 idx 8), grows up
 *   0x0200_0000_0000               stack top (L0 idx 16), grows down
 *   < 0x0400_0000_0000             uaccess validity limit
 */
#define USER_L0_LO      8u              /* first L0 index owned by user */
#define USER_L0_HI      16u             /* one past the last            */

#define USER_CODE_BASE  0x0000010000000000ULL
#define USER_STACK_TOP  0x0000020000000000ULL
#define USER_STACK_SIZE (256u * 1024u)
#define USER_VA_LIMIT   0x0000040000000000ULL

#define PROC_NAME_MAX   16
#define PROC_KSTACK     (16u * 1024u)   /* dedicated kernel-mode stack  */

/* phase 8 IPC resource budgets owned per process (see include/ipc.h) */
#define PROC_SHM_MAX     4u             /* attached shm regions        */
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

#endif /* PROC_H */
