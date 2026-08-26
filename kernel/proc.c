/*
 * proc.c - processes: address spaces, ASIDs, lifecycle.
 *
 * A process owns a TTBR0 root table whose L0 index 0 aliases the
 * kernel's identity-mapped subtree (so EL1 code keeps running
 * unchanged) while indices USER_L0_LO..USER_L0_HI hold the user
 * image. Every root is tagged with an 8-bit ASID carried in
 * TTBR0_EL1[63:56] (TCR.A1 = 1), so switching address spaces is a
 * single system-register write and stale user TLB entries from
 * other processes are filtered by the ASID instead of flushed.
 *
 * Kernel threads keep proc == NULL: their "address space" is the
 * plain boot identity map with reserved ASID 0, which contains no
 * nG=1 pages at all and therefore never collides with a user tag.
 *
 * Each process owns a dedicated kernel stack (PROC_KSTACK,
 * kmalloc'd): exception frames from EL0 land there, and the frame
 * base doubles as the parked SP_EL1 between syscalls (see
 * arch/aarch64/user_entry.S). The static scheduler stack handed to
 * task_create() is only used until the body trampoline runs.
 *
 * exec/spawn build the NEW address space completely before any
 * switch happens: pages are populated through uacc_copy_out against
 * the not-yet-active root, so a failed exec leaves the old image
 * untouched.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "cpu.h"
#include "elf.h"
#include "exceptions.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "panic.h"
#include "proc.h"
#include "signal.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "uaccess.h"
#include "vfs.h"

/* built-in images embedded by arch/aarch64/builtin_imgs.S */
extern const uint8_t builtin_hello_start[];
extern const uint8_t builtin_hello_end[];

struct builtin_image {
    const char  *name;
    const void  *start;
    const void  *end;
};

static const struct builtin_image builtins[] = {
    { "hello", builtin_hello_start, builtin_hello_end },
};

#define PROC_PRIO 10

static struct proc *proc_list;          /* every live proc struct      */
static spinlock_t proc_lock = SPINLOCK_INIT;
static int next_pid = 1;

static struct waitqueue reap_wq;        /* waitpid() sleepers          */

/* assembly entry (arch/aarch64/user_entry.S) */
extern void proc_enter_user(struct trap_frame *tf) __attribute__((noreturn));

/* shared state lock in kernel/task.c (slot states) */
extern spinlock_t task_state_lock;

/* ---- tiny local string helpers (lib has no str* yet) ----------------------- */

static size_t p_strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

static bool p_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void p_strcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;

    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* ---- ASID allocator -------------------------------------------------------- */

/*
 * 8-bit ASIDs; 0 is reserved for the shared kernel context. ASIDs
 * are handed out uniquely until exhausted, then the generation is
 * bumped with a full TLB invalidate and allocation restarts at 1.
 * Freed ASIDs get an immediate per-ASID invalidate so they may be
 * reused within the same generation without aliasing.
 */
#define KERNEL_ASID 0u

static uint8_t  asid_next = 1;
static uint64_t asid_gen = 1;
static bool     asid_in_use[256];

static void tlb_flush_asid(uint8_t asid)
{
    uint64_t arg = (uint64_t)asid << 48;

    __asm__ volatile("tlbi aside1is, %0" :: "r"(arg));
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}

static void tlb_flush_all(void)
{
    __asm__ volatile("tlbi vmalle1is");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}

static bool asid_alloc(uint8_t *out, uint64_t *gen_out)
{
    unsigned start = asid_next, a;
    bool wrapped = false;

    for (a = start; a <= 255u; a++)
        if (!asid_in_use[a])
            goto found;
    for (a = 1; a < start; a++)
        if (!asid_in_use[a])
            goto found;

    /* exhausted: a new generation invalidates every cached ASID tag */
    asid_gen++;
    memset(asid_in_use, 0, sizeof(asid_in_use));
    tlb_flush_all();
    a = 1;
    wrapped = true;

found:
    asid_next = wrapped ? 2u : (a == 255u ? 1u : (uint8_t)(a + 1));
    asid_in_use[a] = true;
    *out = (uint8_t)a;
    *gen_out = asid_gen;
    return true;
}

static void asid_release(uint8_t asid)
{
    if (asid == KERNEL_ASID || !asid_in_use[asid])
        return;
    asid_in_use[asid] = false;
    tlb_flush_asid(asid);
}

/* ---- subsystem init --------------------------------------------------------- */

/*
 * Per-cpu half (secondaries run this from their bring-up path):
 * TCR.A1 = ASIDs come from TTBR0_EL1[63:56], and FP/SIMD traps are
 * lifted at EL0/EL1. There is still no FP state save across context
 * switches, so user binaries must stay general-regs-only for now
 * (the built-in hello does).
 */
void proc_cpu_init(void)
{
    uint64_t tcr;

    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    tcr |= (1ull << 22);
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    __asm__ volatile("msr cpacr_el1, %0" :: "r"((3ull << 20)));

    tlb_flush_all();
}

/* boot-cpu wrapper: applies the per-cpu config once, then reports */
void proc_subsys_init(void)
{
    proc_cpu_init();
    kprintf("proc: address spaces ready (ASIDs from TTBR0)\n");
}

/* ---- context ---------------------------------------------------------------- */

struct proc *proc_current(void)
{
    struct task *t = current_task();

    return t ? t->proc : NULL;
}

void proc_address_space_switch(struct proc *p)
{
    uint64_t ttbr;

    if (p)
        ttbr = p->root_pa | ((uint64_t)p->asid << 56);
    else
        ttbr = vmm_kernel_root();       /* shared map, reserved ASID 0 */

    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr) : "memory");
    __asm__ volatile("isb");
}

/* ---- registry -------------------------------------------------------------- */

static void registry_add(struct proc *p)
{
    daif_state s;

    spin_lock_irqsave(&proc_lock, &s);
    p->next_all = proc_list;
    proc_list = p;
    spin_unlock_irqrestore(&proc_lock, s);
}

static void registry_del(struct proc *p)
{
    daif_state s;

    spin_lock_irqsave(&proc_lock, &s);
    if (proc_list == p) {
        proc_list = p->next_all;
    } else {
        struct proc *it = proc_list;

        while (it && it->next_all != p)
            it = it->next_all;
        if (it)
            it->next_all = p->next_all;
    }
    p->next_all = NULL;
    spin_unlock_irqrestore(&proc_lock, s);
}

static const struct builtin_image *find_builtin(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(builtins); i++)
        if (p_streq(builtins[i].name, name))
            return &builtins[i];
    return NULL;
}

static int pid_alloc(void)
{
    daif_state s;
    int pid;

    spin_lock_irqsave(&proc_lock, &s);
    pid = next_pid++;
    spin_unlock_irqrestore(&proc_lock, s);
    return pid;
}

/* ---- address spaces ---------------------------------------------------------- */

static void space_destroy(struct proc *p)
{
    if (p->root_pa) {
        vmm_root_release(p->root_pa, USER_L0_LO, USER_L0_HI);
        vmm_root_free(p->root_pa);
        p->root_pa = 0;
    }
    asid_release(p->asid);
    p->asid = KERNEL_ASID;
}

/*
 * Map the user stack into `root` and lay out the initial stack:
 * strings on top, then argv/envp vectors plus auxv below them.
 * Every write goes through uacc_copy_out so destination pages are
 * validated exactly like any other user write -- even though this
 * runs before the root becomes active.
 *
 * Returns 0 or -ENOMEM/-EFAULT; on success sets the initial
 * register inputs.
 */
static long stack_setup(paddr_t root,
                        const char *const *argv,
                        const char *const *envp,
                        uint64_t *usp_out, unsigned *argc_out,
                        uint64_t *argv_out, uint64_t *envp_out)
{
    unsigned argc = 0, envc = 0, i;
    size_t str_bytes = 0, total;
    uint64_t sp = USER_STACK_TOP;
    uint64_t base;
    uint64_t argv_vec[8];
    uint64_t envp_vec[4];

    while (argv && argv[argc] && argc < 8) {
        str_bytes += p_strlen(argv[argc]) + 1;
        argc++;
    }
    while (envp && envp[envc] && envc < 4) {
        str_bytes += p_strlen(envp[envc]) + 1;
        envc++;
    }

    total = ALIGN_UP(str_bytes, 16) +
            sizeof(uint64_t) * (argc + 1 + envc + 1) +   /* vectors   */
            4 * sizeof(uint64_t);                        /* auxv pairs*/

    if ((uint64_t)total + 256u > USER_STACK_SIZE)
        return -ENOMEM;

    for (i = 0; i < argc; i++) {
        size_t l = p_strlen(argv[i]) + 1;

        sp -= l;
        argv_vec[i] = sp;
        if (uacc_copy_out(root, sp, argv[i], l))
            return -EFAULT;
    }
    for (i = 0; i < envc; i++) {
        size_t l = p_strlen(envp[i]) + 1;

        sp -= l;
        envp_vec[i] = sp;
        if (uacc_copy_out(root, sp, envp[i], l))
            return -EFAULT;
    }
    sp = ALIGN_DOWN(sp, 16);

    base = ALIGN_DOWN(sp - (sizeof(uint64_t) *
                            (argc + 1 + envc + 1) +
                            4 * sizeof(uint64_t)), 16);

    {
        uint64_t zero = 0;
        uint64_t argc_val = argc;
        uint64_t ev = base + sizeof(uint64_t) +
                      (argc + 1) * sizeof(uint64_t);
        uint64_t av_va = base + sizeof(uint64_t) +
                         (argc + 1 + envc + 1) * sizeof(uint64_t);
        uint64_t auxv[4] = {
            6, PAGE_SIZE,                   /* AT_PAGESZ          */
            0, 0,                           /* AT_NULL            */
        };

        if (uacc_copy_out(root, base, &argc_val, sizeof(argc_val)) ||
            uacc_copy_out(root, base + sizeof(uint64_t),
                          argv_vec, argc * sizeof(uint64_t)) ||
            uacc_copy_out(root, base + sizeof(uint64_t) +
                          argc * sizeof(uint64_t), &zero,
                          sizeof(zero)) ||
            uacc_copy_out(root, ev, envp_vec,
                          envc * sizeof(uint64_t)) ||
            uacc_copy_out(root, ev + envc * sizeof(uint64_t),
                          &zero, sizeof(zero)) ||
            uacc_copy_out(root, av_va, auxv, sizeof(auxv)))
            return -EFAULT;
    }

    *usp_out = base;
    *argc_out = argc;
    *argv_out = base + sizeof(uint64_t);
    *envp_out = base + sizeof(uint64_t) + (argc + 1) * sizeof(uint64_t);
    return 0;
}

/*
 * Load a whole built-in image into `root` and craft the first user
 * register state in `tf`.
 */
static long load_image(const struct builtin_image *bi, paddr_t root,
                       const char *const *argv,
                       const char *const *envp,
                       struct trap_frame *tf, vaddr_t *brk_out)
{
    uint64_t entry;
    uint64_t usp, argv_vec, envp_vec;
    unsigned argc;

    {
        size_t len = (size_t)((const uint8_t *)bi->end -
                              (const uint8_t *)bi->start);

        if (elf_load(root, bi->start, len, &entry, brk_out))
            return -ENOENT;
    }

    if (stack_setup(root, argv, envp, &usp, &argc,
                    &argv_vec, &envp_vec))
        return -EFAULT;

    memset(tf, 0, sizeof(*tf));
    tf->regs[0] = argc;
    tf->regs[1] = argv_vec;
    tf->regs[2] = envp_vec;
    tf->regs[30] = 0;                   /* no return address          */
    tf->sp = usp;
    tf->elr = entry;
    tf->spsr = 0;                       /* EL0t, interrupts enabled   */
    return 0;
}

/* ---- entering / leaving EL0 -------------------------------------------------- */

/* frame base for a fresh kernel-mode trap frame on the proc's kstack */
static struct trap_frame *kstack_frame(struct proc *p)
{
    uintptr_t top = ALIGN_UP((uintptr_t)p->kstack + PROC_KSTACK, 16);

    return (struct trap_frame *)(top - sizeof(struct trap_frame));
}

/*
 * Body of a freshly spawned/exec'd process's task. The task adopts
 * its ->proc itself, closing the create/pick race without extra
 * locking: whichever cpu runs this first sees proc == NULL.
 */
static void proc_task_body(void *raw)
{
    struct proc *p = raw;
    struct task *t = current_task();
    struct trap_frame *tf;

    if (!t->proc)
        t->proc = p;
    else if (t->proc != p)
        panic("proc: slot reuse with live proc");

    proc_address_space_switch(p);

    tf = kstack_frame(p);
    memcpy(tf, &p->entry_tf, sizeof(*tf));
    proc_enter_user(tf);                /* never returns              */
}

/* fork child: same shape, but x0 was already cleared in entry_tf */
static void fork_child_body(void *raw)
{
    struct proc *p = raw;
    struct task *t = current_task();
    struct trap_frame *tf;

    if (!t->proc)
        t->proc = p;
    else if (t->proc != p)
        panic("proc: slot reuse with live proc");

    proc_address_space_switch(p);

    tf = kstack_frame(p);
    memcpy(tf, &p->entry_tf, sizeof(*tf));
    proc_enter_user(tf);                /* returns to fork's caller!  */
}

/* ---- zombie / reap ------------------------------------------------------------- */

/*
 * Mark p zombie, wake reapers. Safe to call for SELF (caller then
 * parks via proc_die) or bookkeeping-only paths. Never frees the
 * task slot here: the exiting task may still be winding down.
 */
static void mark_zombie(struct proc *p, int code, const char *why)
{
    daif_state s;

    p->alive = false;
    p->exit_code = code;
    asid_release(p->asid);
    p->asid = KERNEL_ASID;

    spin_lock_irqsave(&task_state_lock, &s);
    if (p->task && p->task->state != TASK_UNUSED)
        p->task->state = TASK_DEAD;
    spin_unlock_irqrestore(&task_state_lock, s);

    kprintf("[proc] pid %d (%s) %s, code %d\n",
            p->pid, p->name, why, code);

    wait_wake_all(&reap_wq);
}

void proc_die(struct proc *p, int code, const char *why)
{
    mark_zombie(p, code, why);
    proc_address_space_switch(NULL);    /* stop touching user space   */
    task_exit();                        /* never returns              */
}

void proc_do_exit(int code)
{
    struct proc *p = proc_current();

    if (!p)
        panic("exit outside a process");
    proc_die(p, code & 0xff, "exited");
}

static bool reap_one(struct proc *zombie, int *code_out, int *pid_out)
{
    struct task *t = zombie->task;
    daif_state s;

    registry_del(zombie);
    space_destroy(zombie);

    /* phase 7: close the process's files before anything else      */
    vfs_proc_fds_release(zombie);

    if (zombie->kstack) {
        kfree(zombie->kstack);
        zombie->kstack = NULL;
    }

    if (code_out)
        *code_out = zombie->exit_code;
    if (pid_out)
        *pid_out = zombie->pid;

    /* free the task slot only once its context is fully parked     */
    spin_lock_irqsave(&task_state_lock, &s);
    if (t && t->state == TASK_DEAD) {
        t->state = TASK_UNUSED;
        t->proc = NULL;
        t->fn = NULL;
        t->arg = NULL;
    } else if (t) {
        kprintf("[proc] pid %d slot still live at reap (leaked)\n",
                zombie->pid);
    }
    spin_unlock_irqrestore(&task_state_lock, s);

    kfree(zombie);
    return true;
}

/* find next reapable child of `parent` matching want (-1 = any) */
static struct proc *find_zombie(struct proc *parent, int want)
{
    daif_state s;

    spin_lock_irqsave(&proc_lock, &s);
    for (struct proc *it = proc_list; it; it = it->next_all) {
        if (it->parent != parent || it->alive)
            continue;
        if (want >= 0 && it->pid != want)
            continue;
        spin_unlock_irqrestore(&proc_lock, s);
        return it;
    }
    spin_unlock_irqrestore(&proc_lock, s);
    return NULL;
}

static bool has_child(struct proc *parent, int want)
{
    daif_state s;

    spin_lock_irqsave(&proc_lock, &s);
    for (struct proc *it = proc_list; it; it = it->next_all) {
        if (it->parent != parent)
            continue;
        if (want >= 0 && it->pid != want)
            continue;
        spin_unlock_irqrestore(&proc_lock, s);
        return true;
    }
    spin_unlock_irqrestore(&proc_lock, s);
    return false;
}

int proc_poll_reap(int want, int *code_out, int *pid_out)
{
    struct proc *p = proc_current();
    struct proc *z;

    if (!p)
        panic("waitpid outside a process");

    z = find_zombie(p, want);
    if (z) {
        int pid = z->pid;

        reap_one(z, code_out, pid_out);
        return pid;
    }
    return has_child(p, want) ? 0 : -ECHILD;
}

/* predicate for wait_sleep_when: some child is reapable */
struct reap_check {
    struct proc *who;
    int want;
};

static bool reapable_exists(void *ctx)
{
    struct reap_check *rc = ctx;

    return find_zombie(rc->who, rc->want) != NULL;   /* true = keep waiting */
}

int proc_do_waitpid(int want, int *code_out)
{
    struct proc *p = proc_current();
    int pid, code;

    if (!p)
        panic("waitpid outside a process");

    for (;;) {
        switch (proc_poll_reap(want, &code, &pid)) {
        case -ECHILD:
            return -ECHILD;
        case 0:
            break;
        default:
            if (code_out)
                *code_out = code;
            return pid;
        }

        {
            struct reap_check rc = { p, want };

            wait_sleep_when(reapable_exists, &rc, &reap_wq);
        }
    }
}

/* ---- spawn / fork / exec -------------------------------------------------------- */

int proc_spawn(const char *img_name,
               const char *const *argv, const char *const *envp)
{
    const struct builtin_image *bi = find_builtin(img_name);
    struct proc *p;
    struct trap_frame tf;
    vaddr_t brk;
    int slot, pid;
    long r;

    if (!bi)
        return -ENOENT;

    p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->kstack = kmalloc(PROC_KSTACK);
    if (!p->kstack) {
        kfree(p);
        return -ENOMEM;
    }

    p->root_pa = vmm_root_alloc();
    if (!p->root_pa) {
        kfree(p->kstack);
        kfree(p);
        return -ENOMEM;
    }
    asid_alloc(&p->asid, &p->asid_gen);

    r = load_image(bi, p->root_pa, argv, envp, &tf, &brk);
    if (r) {
        space_destroy(p);
        kfree(p->kstack);
        kfree(p);
        return (int)r;
    }
    p->brk = brk;

    pid = pid_alloc();
    p->pid = pid;
    p->alive = true;
    p->sigframe_va = 0;
    p->in_signal = false;
    p->sig_pending = 0;
    p->entry_tf = tf;
    p_strcpy(p->name, img_name, PROC_NAME_MAX);

    /* phase 7: fresh fd table with stdio attached to the console   */
    r = vfs_proc_fds_init(p);
    if (r) {
        space_destroy(p);
        kfree(p->kstack);
        kfree(p);
        return (int)r;
    }

    slot = task_create(img_name, proc_task_body, p, PROC_PRIO);
    if (slot < 0) {
        vfs_proc_fds_release(p);
        space_destroy(p);
        kfree(p->kstack);
        kfree(p);
        return -EAGAIN;
    }
    p->task = &tasks[slot];
    registry_add(p);

    kprintf("[proc] spawned \"%s\" pid %d asid %u root %llx\n",
            p->name, p->pid, p->asid,
            (unsigned long long)p->root_pa);
    return pid;
}

int proc_do_fork(struct trap_frame *tf)
{
    struct proc *parent = proc_current();
    struct proc *child;
    int slot;

    if (!parent)
        panic("fork outside a process");

    child = kzalloc(sizeof(*child));
    if (!child)
        return -ENOMEM;
    child->kstack = kmalloc(PROC_KSTACK);
    if (!child->kstack) {
        kfree(child);
        return -ENOMEM;
    }

    child->root_pa = vmm_root_alloc();
    if (!child->root_pa) {
        kfree(child->kstack);
        kfree(child);
        return -ENOMEM;
    }
    asid_alloc(&child->asid, &child->asid_gen);

    if (vmm_copy_space(child->root_pa, parent->root_pa,
                       USER_L0_LO, USER_L0_HI)) {
        space_destroy(child);
        kfree(child->kstack);
        kfree(child);
        return -ENOMEM;
    }

    child->pid = pid_alloc();
    child->alive = true;
    child->parent = parent;
    p_strcpy(child->name, parent->name, PROC_NAME_MAX);
    memcpy(child->sig_handler, parent->sig_handler,
           sizeof(child->sig_handler)); /* POSIX: handlers inherited  */
    child->sig_pending = 0;             /* pending is NOT inherited   */
    child->in_signal = false;
    child->brk = parent->brk;

    /* phase 7: fork shares open file descriptions (dup refs)       */
    vfs_proc_fds_inherit(child, parent);

    /* the child's first user moment: same PC/SP/regs, but retval 0 */
    memcpy(&child->entry_tf, tf, sizeof(*tf));
    child->entry_tf.regs[0] = 0;

    slot = task_create(child->name, fork_child_body, child, PROC_PRIO);
    if (slot < 0) {
        space_destroy(child);
        kfree(child->kstack);
        kfree(child);
        return -EAGAIN;
    }
    child->task = &tasks[slot];
    registry_add(child);

    kprintf("[proc] fork: pid %d -> pid %d\n",
            parent->pid, child->pid);
    return child->pid;
}

long proc_do_exec(const char *name,
                  const char *const *argv, const char *const *envp)
{
    const struct builtin_image *bi = find_builtin(name);
    struct proc *p = proc_current();
    paddr_t nroot, oroot;
    uint8_t nasid, oasid;
    uint64_t ngen;
    struct trap_frame tf;
    struct trap_frame *frame;
    vaddr_t nbrk, obrk;
    long r;

    if (!p)
        panic("exec outside a process");
    if (!bi)
        return -ENOENT;

    nroot = vmm_root_alloc();
    if (!nroot)
        return -ENOMEM;
    asid_alloc(&nasid, &ngen);

    r = load_image(bi, nroot, argv, envp, &tf, &nbrk);
    if (r) {
        vmm_root_release(nroot, USER_L0_LO, USER_L0_HI);
        vmm_root_free(nroot);
        asid_release(nasid);
        return r;
    }

    /* commit point: swap address spaces, then tear the old one down */
    oroot = p->root_pa;
    oasid = p->asid;
    obrk = p->brk;

    p->root_pa = nroot;
    p->asid = nasid;
    p->asid_gen = ngen;
    p->brk = nbrk;

    /* POSIX-lite: handlers and pending signals reset across exec   */
    memset(p->sig_handler, 0, sizeof(p->sig_handler));
    p->sig_pending = 0;
    p->in_signal = false;
    p->sigframe_va = 0;

    p_strcpy(p->name, name, PROC_NAME_MAX);

    vmm_root_release(oroot, USER_L0_LO, USER_L0_HI);
    vmm_root_free(oroot);
    asid_release(oasid);
    (void)obrk;

    proc_address_space_switch(p);

    frame = kstack_frame(p);
    memcpy(frame, &tf, sizeof(*frame));
    kprintf("[proc] pid %d exec \"%s\"\n", p->pid, name);
    proc_enter_user(frame);             /* never returns              */
}

/* ---- kill ------------------------------------------------------------------------- */

/*
 * Marks the signal pending on the target. Delivery happens at the
 * target's next kernel->EL0 boundary (syscall return or IRQ return);
 * unhandled fatal signals terminate there. There is deliberately no
 * cross-cPU stack surgery: a blocked task learns of the signal when
 * it next reaches user mode (no EINTR machinery yet -- documented in
 * docs/PHASE_5.md).
 */
int proc_do_kill(int pid, unsigned sig)
{
    daif_state s;
    struct proc *it, *target = NULL;

    if (sig == 0 || sig >= NSIG)
        return -EINVAL;

    spin_lock_irqsave(&proc_lock, &s);
    for (it = proc_list; it; it = it->next_all) {
        if (it->pid == pid) {
            target = it;
            break;
        }
    }
    if (target && target->alive)
        target->sig_pending |= (1u << (sig - 1));
    spin_unlock_irqrestore(&proc_lock, s);

    return target ? (target->alive ? 0 : -ESRCH) : -ESRCH;
}

/* ---- faults ---------------------------------------------------------------------- */

void proc_user_fault(struct trap_frame *tf, uint64_t esr, uint64_t far)
{
    struct proc *p = proc_current();
    uint64_t ec = (esr >> 26) & 0x3f;

    kprintf("[proc] FAULT pid %d (%s): EC=%llx FAR=%016llx "
            "ELR=%016llx\n",
            p ? p->pid : -1, p ? p->name : "?",
            (unsigned long long)ec,
            (unsigned long long)far,
            (unsigned long long)tf ? tf->elr : 0);

    if (!p)
        panic("user fault outside a process");
    proc_die(p, 128 + SIGSEGV, "faulted");
}
