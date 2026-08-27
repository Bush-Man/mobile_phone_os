#ifndef PM_H
#define PM_H

#include <stdbool.h>
#include <stdint.h>

struct platform_info;

/*
 * Power management core (phase 10, items 54+57).
 *
 * Idle governor: the scheduler's empty-run branch calls
 * pm_cpu_idle(); today's policy answer is always WFI -- the port's
 * GIC stays armed across it, so timer ticks, UART RX and VirtIO
 * interrupts all act as wake sources natively ("wake sources wired
 * to GIC" holds structurally here; boards get per-depth wake masks
 * in their bring-up data later).
 *
 * Display suspension: pure decision function fed (now, last input
 * activity, timeout) makes both the live engine and the phase-10
 * selftest share one deterministic code path. Suspend presents a
 * black frame ONCE and stops refreshing; resume restores control
 * flow immediately while content repaint stays a client duty until
 * a compositor exists (documented contract).
 */

/* ---- subsystem ------------------------------------------------------------------ */

void pm_init(const struct platform_info *plat);

/* ---- idle governor ---------------------------------------------------------------- */

enum pm_idle_depth {
    PM_IDLE_WFI = 0,
    PM_IDLE_DEEP_RESERVED,          /* no board implements this yet */
};

/* scheduler idle branch entry point (irqs armed, no locks held)   */
void pm_cpu_idle(uint64_t cpu);

/* policy hook: currently always WFI (extension point)             */
enum pm_idle_depth pm_select_depth(void);

unsigned pm_wfi_count(void);

/* ---- wake sources ----------------------------------------------------------------- */

int      pm_wake_source_note(unsigned intid, const char *name);
unsigned pm_wake_source_count(void);
const char *pm_wake_source_name(unsigned idx); /* NULL past end    */

/* ---- activity + display power ----------------------------------------------------- */

/* every input event funnels here (input.c); IRQ-safe, cheap        */
void pm_input_activity(void);

uint64_t pm_last_activity_ms(void);
void     pm_display_set_timeout(uint32_t ms);
uint32_t pm_display_timeout(void);

enum pm_disp_state { PM_DISP_ON = 0, PM_DISP_SUSPENDED };

enum pm_disp_action {
    PM_DISP_STAY        = 0,
    PM_DISP_SUSPEND_NOW = 1,
    PM_DISP_RESUME_NOW  = 2,
};

/*
 * Pure policy: identical inputs -> identical outputs. The runtime
 * engine and the selftest exercise exactly this math. A suspended
 * display resumes ONLY when an event set the pending flag; a live
 * display suspends after timeout_ms of silence (0 disables).
 */
enum pm_disp_action pm_display_decide(uint64_t now_ms,
                                      uint64_t last_activity_ms,
                                      bool suspended,
                                      bool pending,
                                      uint32_t timeout_ms);

/* housekeeping cadence: apply whatever decide() says               */
void pm_display_tick(uint64_t now_ms);

enum pm_disp_state pm_display_state(void);
uint64_t pm_suspend_count(void);
uint64_t pm_resume_count(void);

#endif /* PM_H */