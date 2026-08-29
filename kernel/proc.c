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
#include "crash.h"
#include "elf.h"
#include "exceptions.h"
#include "ipc.h"
#include "usabi.h"
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
#include "time.h"
#include "uaccess.h"
#include "vfs.h"

/* built-in images embedded by arch/aarch64/builtin_imgs.S */
extern const uint8_t builtin_hello_start[];
extern const uint8_t builtin_hello_end[];
extern const uint8_t builtin_ipcdemo_start[];
extern const uint8_t builtin_ipcdemo_end[];
extern const uint8_t builtin_evreader_start[];
extern const uint8_t builtin_evreader_end[];
extern const uint8_t builtin_netcli_start[];
extern const uint8_t builtin_netcli_end[];
/* phase 14: userspace foundation */
extern const uint8_t builtin_init_start[];
extern const uint8_t builtin_init_end[];
extern const uint8_t builtin_sh_start[];
extern const uint8_t builtin_sh_end[];
extern const uint8_t builtin_batteryd_start[];
extern const uint8_t builtin_batteryd_end[];
extern const uint8_t builtin_udevd_start[];
extern const uint8_t builtin_udevd_end[];
extern const uint8_t builtin_timed_start[];
extern const uint8_t builtin_timed_end[];
extern const uint8_t builtin_libctest_start[];
extern const uint8_t builtin_libctest_end[];
extern const uint8_t builtin_crasher_start[];
extern const uint8_t builtin_crasher_end[];
/* phase 15: compositor + phone apps */
extern const uint8_t builtin_compositor_start[];
extern const uint8_t builtin_compositor_end[];
extern const uint8_t builtin_dialer_start[];
extern const uint8_t builtin_dialer_end[];
extern const uint8_t builtin_msgs_start[];
extern const uint8_t builtin_msgs_end[];
extern const uint8_t builtin_contacts_start[];
extern const uint8_t builtin_contacts_end[];
extern const uint8_t builtin_clock_start[];
extern const uint8_t builtin_clock_end[];
extern const uint8_t builtin_calc_start[];
extern const uint8_t builtin_calc_end[];
extern const uint8_t builtin_settings_start[];
extern const uint8_t builtin_settings_end[];
extern const uint8_t builtin_uitest_start[];
extern const uint8_t builtin_uitest_end[];

struct builtin_image {
    const char  *name;
    const void  *start;
    const void  *end;
};

static const struct builtin_image builtins[] = {
    { "hello", builtin_hello_start, builtin_hello_end },
    { "ipcdemo", builtin_ipcdemo_start, builtin_ipcdemo_end },
    { "evreader", builtin_evreader_start, builtin_evreader_end },
    { "netcli", builtin_netcli_start, builtin_netcli_end },
    /* phase 14: libc-linked programs (crt0 + userspace/libc)      */
    { "init", builtin_init_start, builtin_init_end },
    { "sh", builtin_sh_start, builtin_sh_end },
    { "batteryd", builtin_batteryd_start, builtin_batteryd_end },
    { "udevd", builtin_udevd_start, builtin_udevd_end },
    { "timed", builtin_timed_start, builtin_timed_end },
    { "libctest", builtin_libctest_start, builtin_libctest_end },
    { "crasher", builtin_crasher_start, builtin_crasher_end },
    /* phase 15: compositor + phone apps                            */
    { "compositor", builtin_compositor_start,
      builtin_compositor_end },
    { "dialer", builtin_dialer_start, builtin_dialer_end },
    { "msgs", builtin_msgs_start, builtin_msgs_end },
    { "contacts", builtin_contacts_start, builtin_contacts_end },
    { "clock", builtin_clock_start, builtin_clock_end },
    { "calc", builtin_calc_start, builtin_calc_end },
    { "settings", builtin_settings_start, builtin_settings_end },
    { "uitest", builtin_uitest_start, builtin_uitest_end },
};

#define PROC_PRIO 10

static struct proc *proc_list;          /* every live proc struct      */
static spinlock_t proc_lock = SPINLOCK_INIT;
static int next_pid = 1;

static struct proc *init_proc;          /* orphan reaper (PID 1)       */

/*
 * Phase 16 (item 85): per-boot PRNG for user-VA randomization.
 * Seeded from the architected counter + boot jiffies in
 * proc_subsys_init -- neither is a secret, but together they make
 * the layout unpredictable across reboots, which defeats the
 * fixed-offset class of exploits on a phone.
 */
static uint64_t kaslr_seed;

static uint64_t kaslr_next(void)
{
    kaslr_seed ^= kaslr_seed << 13;
    kaslr_seed ^= kaslr_seed >> 7;
    kaslr_seed ^= kaslr_seed << 17;
    return kaslr_seed;
}

/* randomize the private-mmap window base (max ~4 GiB into the
 * 512 GiB window; page-aligned)                                     */
static vaddr_t kaslr_mmap_base(void)
{
    return USER_MMAP_BASE +
           (vaddr_t)(kaslr_next() % 0x100000ull) * PAGE_SIZE;
}

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
    /* phase 16: seed the ASLR PRNG from two independent boot-time
     * sources; the counter runs from power-on, jiffies from the
     * first timer IRQs -- their low bits differ every boot       */
    kaslr_seed = time_counter_value() ^ ((uint64_t)jiffies_read() << 32);
    if (!kaslr_seed)
        kaslr_seed = 0x9e3779b97f4a7c15ULL;

    proc_cpu_init();
    kprintf("proc: address spaces ready (ASIDs from TTBR0)\n");
    kprintf("proc: user-VA randomization armed\n");
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

    /*
     * Map the full stack reservation first: uacc_copy_out below
     * validates against the root's page tables, so nothing can be
     * written until the pages exist. RW+X on purpose -- the signal
     * trampoline (kernel/signal.c) executes from the user stack.
     * Teardown is free: the pages live under root's L0 indices and
     * vmm_root_release() reclaims them with the rest of the space.
     */
    for (uint64_t sva = USER_STACK_TOP - USER_STACK_SIZE;
         sva < USER_STACK_TOP; sva += PAGE_SIZE) {
        paddr_t fr = pmm_alloc();

        if (!fr)
            return -ENOMEM;             /* root teardown unwinds us */
        if (vmm_map_at(root, sva, fr,
                       VM_READ | VM_WRITE | VM_EXEC | VM_USER)) {
            pmm_free(fr);
            return -ENOMEM;
        }
    }

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

/* ---- init / orphan reparenting (phase 14) --------------------------------- */

void proc_note_init_pid(int pid)
{
    daif_state s;
    struct proc *found = NULL;

    spin_lock_irqsave(&proc_lock, &s);
    for (struct proc *it = proc_list; it; it = it->next_all)
        if (it->pid == pid && it->alive) {
            found = it;
            break;
        }
    spin_unlock_irqrestore(&proc_lock, s);

    if (found) {
        init_proc = found;
        kprintf("[proc] init is pid %d (orphan reaper)\n", pid);
    } else {
        kprintf("[proc] init registration failed (pid %d)\n", pid);
    }
}

struct proc *proc_init_proc(void)
{
    return init_proc;
}

/*
 * Phase 16: registry lookup by pid for kernel-side probes (the
 * release selftest walks a live process's page tables to audit W^X
 * and read its randomized mmap base). Zombies qualify: their
 * mappings are intact until the reaper runs. NULL when unknown.
 */
struct proc *proc_by_pid(int pid)
{
    daif_state s;
    struct proc *found = NULL;

    spin_lock_irqsave(&proc_lock, &s);
    for (struct proc *it = proc_list; it; it = it->next_all)
        if (it->pid == pid) {
            found = it;
            break;
        }
    spin_unlock_irqrestore(&proc_lock, s);
    return found;
}

int proc_pid_of_name(const char *name)
{
    daif_state s;
    int pid = -1;

    spin_lock_irqsave(&proc_lock, &s);
    for (struct proc *it = proc_list; it; it = it->next_all) {
        if (it->alive && p_streq(it->name, name)) {
            pid = it->pid;
            break;
        }
    }
    spin_unlock_irqrestore(&proc_lock, s);
    return pid;
}

/*
 * Phase 14: fill up to `max` psinfo records (usabi.h layout) for the
 * SYS_psinfo report -- every proc in the registry, zombies included
 * (flagged PSINFO_ZOMBIE, so `ps` can show "Z <defunct>" entries
 * that have not been reaped yet). Kernel threads never appear: they
 * have no proc struct. Oldest-first, pid order == spawn order.
 */
unsigned proc_psinfo_fill(struct psinfo_entry *ents, unsigned max)
{
    daif_state s;
    unsigned out = 0;

    if (!ents)
        return 0;

    spin_lock_irqsave(&proc_lock, &s);
    for (struct proc *it = proc_list; it && out < max; it = it->next_all) {
        ents[out].pid = (uint32_t)it->pid;
        ents[out].ppid = (it->parent && it->parent->alive)
                             ? (uint32_t)it->parent->pid : 0u;
        ents[out].flags = it->alive ? PSINFO_ALIVE : PSINFO_ZOMBIE;
        memset(ents[out].name, 0, sizeof(ents[out].name));
        kstrlcpy(ents[out].name, it->name, sizeof(ents[out].name));
        out++;
    }
    spin_unlock_irqrestore(&proc_lock, s);
    return out;
}

/*
 * Children of the dying `p` move to init so their zombies stay
 * reapable. Children whose new parent is init get SIGCHLD armed on
 * init's pending mask: the kernel has no waitpid-any notification
 * hook yet, so this is the wake-up nudge that tells init a child of
 * ANY kind changed state (its own or adopted). Called with
 * proc_lock held.
 */
static void reparent_children_locked(struct proc *p)
{
    if (!init_proc || init_proc == p)
        return;

    for (struct proc *it = proc_list; it; it = it->next_all) {
        if (it->parent == p && it != p) {
            it->parent = init_proc;
            init_proc->sig_pending |= (1u << (SIGCHLD - 1));
        }
    }
}

/* ---- zombie / reap ------------------------------------------------------------- */

/*
 * Mark p zombie, wake reapers. Safe to call for SELF (caller then
 * parks via proc_die) or bookkeeping-only paths.
 *
 * Note the deliberate sequencing contract reap_one() relies on
 * (phase 5 design, kept): ->alive drops here while the task slot is
 * only MARKED dead -- its context is guaranteed fully parked by the
 * time reclaim happens because task_exit() re-marks DEAD after the
 * final sched_park() handshake. Kernel threads have proc == NULL,
 * so reaping never depends on the REAPER having a task context --
 * only the dying process must own one.
 */
static void mark_zombie(struct proc *p, int code, const char *why)
{
    daif_state s;

    p->alive = false;
    p->exit_code = code;

    {
        daif_state sl;

        spin_lock_irqsave(&proc_lock, &sl);
        reparent_children_locked(p);
        spin_unlock_irqrestore(&proc_lock, sl);
    }

    asid_release(p->asid);
    p->asid = KERNEL_ASID;

    /*
     * Phase 14: the process died -- its THREADS die with it. Marking
     * them DEAD stops dispatch immediately; ones currently running
     * on another cpu stop at their next park (wakers and the irq
     * return path now refuse to re-queue DEAD tasks). Their slots
     * are reclaimed later by proc_threads_reclaim().
     */
    spin_lock_irqsave(&task_state_lock, &s);
    if (p->task && p->task->state != TASK_UNUSED)
        p->task->state = TASK_DEAD;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];

        if (t->t_kstack && t->proc == p && t != p->task &&
            t->state != TASK_UNUSED)
            t->state = TASK_DEAD;
    }
    spin_unlock_irqrestore(&task_state_lock, s);

    kprintf("[proc] pid %d (%s) %s, code %d\n",
            p->pid, p->name, why, code);

    /* phase 14: persist the crash record before the space is gone */
    if (p_streq(why, "faulted") || p_streq(why, "killed by signal"))
        crash_record(p, code, why);

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

    /*
     * Phase 8 IPC teardown FIRST: shared-memory mappings must be
     * removed through the process's root table while it still
     * exists, and queue handles dropped so unreferenced queues can
     * free themselves. Never called for kernel threads (no proc).
     */
    ipc_proc_exit(zombie);

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

/*
 * Phase 14: a threaded process is only reapable once every one of
 * its threads is DEAD *and parked* (context quiescent) -- reap_one
 * destroys the address space, and a still-running thread would be
 * executing on freed page tables. The predicate is monotone thanks
 * to the no-resurrection wakers: once true it stays true.
 */
static bool threads_settled(const struct proc *p)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        const struct task *t = &tasks[i];

        if (t->t_kstack && t->proc == p &&
            !(t->state == TASK_DEAD && t->parked))
            return false;
    }
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
        if (!threads_settled(it))
            continue;               /* threads still draining      */
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

/*
 * Phase 14: reap by pid from KERNEL context (selftest/demo tasks
 * have no proc of their own). The safety argument mirrors reap_one:
 * ->alive is already false, so nobody else can be reaping this
 * zombie; the DEAD-marked task slot is only freed once its context
 * is fully parked, which mark_zombie's TASK_DEAD marking plus the
 * dying task's own task_exit() handshake guarantee.
 */
int proc_kernel_reap(int pid, int *code_out)
{
    struct proc *z = NULL;
    daif_state s;

    spin_lock_irqsave(&proc_lock, &s);
    for (struct proc *it = proc_list; it; it = it->next_all) {
        if (it->pid == pid) {
            if (it->alive) {
                spin_unlock_irqrestore(&proc_lock, s);
                return 0;               /* still running            */
            }
            z = it;
            break;
        }
    }
    spin_unlock_irqrestore(&proc_lock, s);

    if (!z)
        return -ECHILD;
    if (!threads_settled(z))
        return 0;                       /* threads still draining    */

    reap_one(z, code_out, NULL);
    return pid;
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

    /*
     * Phase 8: a KERNEL thread has no proc context and therefore no
     * children (spawn now parents every child to its creator), so
     * there is nothing to wait for -- fail gracefully with -ESRCH
     * instead of panicking. Process-context waiters keep the old
     * "outside a process" panic, which remains a genuine bug trap.
     */
    if (!p)
        return -ESRCH;

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
    struct proc *parent = proc_current();   /* may be NULL (boot) */
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
    p->brk_floor = brk;                 /* phase 14: SYS_brk floor  */
    p->mmap_next = kaslr_mmap_base();   /* phase 16: ASLR'd mmap base */

    pid = pid_alloc();
    p->pid = pid;
    p->alive = true;
    p->parent = parent;     /* phase 8: children are reapable by their
                             * actual spawner (NULL only pre-scheduler) */
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

    /*
     * Phase 14: reserve the task slot while it stays invisible to
     * the scheduler, link ->proc / ->task, THEN publish it READY --
     * a cross-cpu dispatch of an unlinked slot would otherwise run
     * on the wrong address space.
     */
    slot = task_create_deferred(img_name, proc_task_body, p, PROC_PRIO);
    if (slot < 0) {
        vfs_proc_fds_release(p);
        space_destroy(p);
        kfree(p->kstack);
        kfree(p);
        return -EAGAIN;
    }
    p->task = &tasks[slot];
    p->task->name = p->name;            /* stable name for psinfo  */
    registry_add(p);
    task_launch(slot);

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
    child->brk_floor = parent->brk_floor;   /* phase 14: heap floor  */
    child->mmap_next = parent->mmap_next;   /* private maps inherit */

    /* phase 7: fork shares open file descriptions (dup refs)       */
    vfs_proc_fds_inherit(child, parent);

    /* the child's first user moment: same PC/SP/regs, but retval 0 */
    memcpy(&child->entry_tf, tf, sizeof(*tf));
    child->entry_tf.regs[0] = 0;

    /* phase 14: deferred-create + launch (same race argument)      */
    slot = task_create_deferred(child->name, fork_child_body, child,
                                PROC_PRIO);
    if (slot < 0) {
        space_destroy(child);
        kfree(child->kstack);
        kfree(child);
        return -EAGAIN;
    }
    child->task = &tasks[slot];
    child->task->name = child->name;    /* stable name for psinfo  */
    registry_add(child);
    task_launch(slot);

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

    /*
     * Phase 14: exec replaces the address space while the caller's
     * THREADS would still be running on it. pthread-lite programs
     * simply do not exec while threaded (the libc never does);
     * refusing is cheaper and safer than cross-cpu thread surgery.
     */
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];

        if (t->t_kstack && t->proc == p && t != p->task &&
            t->state != TASK_UNUSED && t->state != TASK_DEAD)
            return -EBUSY;
    }

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
    p->brk_floor = nbrk;                 /* phase 14: heap floor    */
    p->mmap_next = kaslr_mmap_base();    /* phase 16: fresh layout  */

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

/* ---- threads (pthread-lite backend, phase 14) ------------------------------ */

/* frame base for a fresh EL1 frame on a THREAD's own kstack */
static struct trap_frame *kstack_frame_thread(struct task *t);

/*
 * Thread body: runs on the thread's dedicated kstack; first entry
 * into user mode goes through the standard proc_enter_user with the
 * pc/usp/arg frame built below (SPSR EL0t). From then on the thread
 * is an ordinary user task that happens to share the leader's proc
 * (and therefore its address space, fd table and pid).
 */
static void thread_body(void *raw)
{
    struct task *t = raw;

    if (t->proc != proc_current())
        panic("thread: slot/proc mismatch at entry");

    proc_address_space_switch(t->proc);

    /*
     * The entry frame was written onto this thread's kstack by
     * proc_thread_create before the launch -- nothing races us to
     * that stack, it belongs to this thread alone. Install it and
     * drop into EL0.
     */
    proc_enter_user(kstack_frame_thread(t));
}

/* frame base for a fresh EL1 frame on a THREAD's own kstack */
static struct trap_frame *kstack_frame_thread(struct task *t)
{
    uintptr_t top = ALIGN_UP((uintptr_t)t->t_kstack + PROC_KSTACK, 16);

    return (struct trap_frame *)(top - sizeof(struct trap_frame));
}

int proc_thread_create(uint64_t pc, uint64_t usp, uint64_t arg)
{
    struct proc *p = proc_current();
    struct task *t;
    struct trap_frame *tf;
    int slot;

    if (!p || !p->alive)
        return -EINVAL;

    /*
     * The user stack must already be mapped by the caller (pthread
     * allocates it through mmap). No kernel validation of usp/pc:
     * they are plain user addresses the thread faults on if wrong
     * (handled like any other user fault).
     */
    slot = task_create_deferred(p->name, thread_body, NULL, PROC_PRIO);
    if (slot < 0)
        return -EAGAIN;

    t = &tasks[slot];
    t->t_kstack = kmalloc(PROC_KSTACK);
    if (!t->t_kstack) {
        daif_state s;

        spin_lock_irqsave(&task_state_lock, &s);
        t->state = TASK_UNUSED;         /* hand the slot back      */
        t->fn = NULL;
        t->arg = NULL;
        spin_unlock_irqrestore(&task_state_lock, s);
        return -ENOMEM;
    }

    t->proc = p;                        /* SHARES the leader's proc  */
    t->arg = t;                         /* thread_body deref's this  */
    t->name = "thread";                 /* stable rodata label       */

    /*
     * Build the first EL0 frame at the top of the thread's kstack;
     * thread_body passes this exact address to proc_enter_user.
     */
    tf = kstack_frame_thread(t);
    memset(tf, 0, sizeof(*tf));
    tf->regs[0] = arg;
    tf->regs[30] = 0;
    tf->sp = usp;
    tf->elr = pc;
    tf->spsr = 0;                       /* EL0t                       */

    task_launch(slot);

    kprintf("[proc] pid %d thread tid %d @%llx\n",
            p->pid, slot, (unsigned long long)pc);
    return slot;
}

void proc_thread_exit(void)
{
    struct task *t = current_task();

    if (!t || !t->t_kstack)
        panic("thread_exit outside a process thread");

    proc_address_space_switch(NULL);    /* stop touching user space  */
    proc_threads_reclaim();             /* peers' slots, not ours    */
    task_exit();                        /* parks; reclaim comes later */
}

void proc_threads_reclaim(void)
{
    bool freed = false;

    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        daif_state s;

        if (!t->t_kstack || t->state != TASK_DEAD || !t->parked)
            continue;                   /* live, or not yet quiescent */

        spin_lock_irqsave(&task_state_lock, &s);
        t->state = TASK_UNUSED;
        t->proc = NULL;
        t->fn = NULL;
        t->arg = NULL;
        t->parked = false;
        spin_unlock_irqrestore(&task_state_lock, s);

        kfree(t->t_kstack);
        t->t_kstack = NULL;
        freed = true;
    }

    /*
     * A settled thread may have been the last thing between a
     * threaded zombie and its reaper -- kick the reap wait queue so
     * init's waitpid(-1) retries (housekeeping calls this every
     * couple of ms; SYS_thread_exit callers get the kick inline).
     */
    if (freed)
        wait_wake_all(&reap_wq);
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
