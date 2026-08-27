/*
 * pm.c - idle governor + display suspend engine (phase 10).
 *
 * Locking: one subsystem spinlock guards counters/state. The idle
 * hook runs from the scheduler loop with nothing held (pre-existing
 * property of the WFI branch it replaces). The display decision is
 * a pure function of timestamps -- no locks inside it.
 *
 * Wake-on-touch: input_push() stamps activity; if the display was
 * suspended, push sets pending_resume and the next housekeeping
 * tick performs the actual present() round-trip in task context --
 * an IRQ/tasklet context must never enter gpu_cmd (it would trip
 * the "before entering the scheduler" panic guard).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "cpu.h"
#include "fb.h"
#include "input.h"
#include "irq.h"
#include "lib.h"
#include "platform.h"
#include "pm.h"
#include "spinlock.h"
#include "time.h"
#include "virtio_gpu.h"

#define PM_WAKE_MAX 8u

static struct {
    spinlock_t lock;

    /* idle governor */
    uint64_t wfi_count[NR_CPUS];
    unsigned wake_ids[PM_WAKE_MAX];
    const char *wake_names[PM_WAKE_MAX];
    unsigned nwakes;
} pm = {
    .lock = SPINLOCK_INIT,
};

/* ---- init --------------------------------------------------------------------- */

void pm_init(const struct platform_info *plat)
{
    /* everything zeroes in .bss, which is exactly SPINLOCK_INIT
     * semantics for our lock word -- no explicit assign needed     */
    memset(&pm, 0, sizeof(pm));
    (void)plat;
}

/* ---- idle governor ------------------------------------------------------------- */

enum pm_idle_depth pm_select_depth(void)
{
    /*
     * Board bring-up (phase 10+ on real hardware) will consult a
     * per-depth validity table + latency budget here. On QEMU the
     * only sensible entry is WFI: virtio interrupts act as wakes
     * because the GIC stays armed across it by construction.
     */
    return PM_IDLE_WFI;
}

void pm_cpu_idle(uint64_t cpu)
{
    daif_state s;

    spin_lock_irqsave(&pm.lock, &s);
    if (cpu < NR_CPUS)
        pm.wfi_count[cpu]++;
    spin_unlock_irqrestore(&pm.lock, s);

    __asm__ volatile("wfi");
}

unsigned pm_wfi_count(void)
{
    uint64_t sum = 0;
    daif_state s;

    spin_lock_irqsave(&pm.lock, &s);
    for (uint64_t c = 0; c < NR_CPUS; c++)
        sum += pm.wfi_count[c];
    spin_unlock_irqrestore(&pm.lock, s);
    return (unsigned)sum;
}

int pm_wake_source_note(unsigned intid, const char *name)
{
    daif_state s;
    int r = -1;

    spin_lock_irqsave(&pm.lock, &s);
    if (pm.nwakes < PM_WAKE_MAX) {
        pm.wake_ids[pm.nwakes]   = intid;
        pm.wake_names[pm.nwakes] = name;
        pm.nwakes++;
        r = 0;
    }
    spin_unlock_irqrestore(&pm.lock, s);
    return r;
}

unsigned pm_wake_source_count(void)
{
    return pm.nwakes;                   /* set-once table             */
}

const char *pm_wake_source_name(unsigned idx)
{
    return idx < pm.nwakes ? pm.wake_names[idx] : NULL;
}

/* ---- activity + display power -------------------------------------------------- */

static struct {
    uint32_t timeout_ms;
    volatile uint64_t last_activity_ms;
    enum pm_disp_state state;
    bool pending_resume;
    uint64_t suspends, resumes;
} disp = {
    .timeout_ms        = 30000u,        /* default 30 s               */
    .state             = PM_DISP_ON,
};

void pm_input_activity(void)
{
    daif_state s;
    uint64_t now = time_uptime_ms();

    spin_lock_irqsave(&pm.lock, &s);
    disp.last_activity_ms = now;

    if (disp.state == PM_DISP_SUSPENDED && !disp.pending_resume)
        disp.pending_resume = true;     /* applied in task context    */
    spin_unlock_irqrestore(&pm.lock, s);
}

uint64_t pm_last_activity_ms(void)
{
    daif_state s;
    uint64_t v;

    spin_lock_irqsave(&pm.lock, &s);
    v = disp.last_activity_ms;
    spin_unlock_irqrestore(&pm.lock, s);
    return v;
}

void pm_display_set_timeout(uint32_t ms)
{
    daif_state s;

    spin_lock_irqsave(&pm.lock, &s);
    disp.timeout_ms = ms;
    spin_unlock_irqrestore(&pm.lock, s);
}

uint32_t pm_display_timeout(void)
{
    return disp.timeout_ms;             /* single word: race-free     */
}

enum pm_disp_state pm_display_state(void)
{
    return disp.state;                  /* aligned single read        */
}

uint64_t pm_suspend_count(void)   { return disp.suspends; }
uint64_t pm_resume_count(void)    { return disp.resumes;  }

/* ---- the pure policy ------------------------------------------------------------ */

enum pm_disp_action pm_display_decide(uint64_t now_ms,
                                      uint64_t last_activity_ms,
                                      bool suspended,
                                      bool pending,
                                      uint32_t timeout_ms)
{
    if (suspended)
        return pending ? PM_DISP_RESUME_NOW : PM_DISP_STAY;

    if (timeout_ms == 0)
        return PM_DISP_STAY;            /* zero disables blanking     */

    /* unsigned arithmetic handles jiffy wraparound naturally       */
    if (now_ms - last_activity_ms >= (uint64_t)timeout_ms)
        return PM_DISP_SUSPEND_NOW;

    return PM_DISP_STAY;
}

/*
 * Apply the decision. Runs only from housekeeping (task context):
 * both suspend and resume go through fb/virtio-gpu present paths
 * that must not be reached from IRQ context.
 */
void pm_display_tick(uint64_t now_ms)
{
    struct fb_canvas cv;
    enum pm_disp_action act;
    daif_state s;
    bool do_suspend = false, do_resume = false;

    spin_lock_irqsave(&pm.lock, &s);
    {
        act = pm_display_decide(now_ms, disp.last_activity_ms,
                                disp.state == PM_DISP_SUSPENDED,
                                disp.pending_resume,
                                disp.timeout_ms);

        switch (act) {
        case PM_DISP_SUSPEND_NOW:
            do_suspend = true;
            break;
        case PM_DISP_RESUME_NOW:
            do_resume         = true;
            disp.pending_resume = false;
            break;
        default:
            break;
        }
    }
    spin_unlock_irqrestore(&pm.lock, s);

    if (do_resume) {
        memset(&cv, 0, sizeof(cv));
        if (fb_active() == NULL)
            kprintf("display: resume (no canvas attached)\n");
        else
            kprintf("display: resumed on input (%llums idle)\n",
                    (unsigned long long)(now_ms - disp.last_activity_ms));
        disp.resumes++;
        disp.state = PM_DISP_ON;
        return;
    }

    if (!do_suspend)
        return;

    memset(&cv, 0, sizeof(cv));
    if (fb_present() && fb_claim_default(&cv)) {
        /* present a black frame; content repaint is client duty   */
        fb_fill_rect((struct fb_canvas *)fb_active(), 0, 0,
                     fb_active()->width, fb_active()->height,
                     fb_rgb(0, 0, 0));
        if (fb_virtio_gpu_present())
            kprintf("display: suspend FAILED at present\n");
        else
            kprintf("display: suspended after %llums idle\n",
                    (unsigned long long)(now_ms - disp.last_activity_ms));
    } else if (!fb_present()) {
        kprintf("display: no display attached, skip suspend log\n");
    }

    disp.suspends++;
    disp.state = PM_DISP_SUSPENDED;
}

