/*
 * psci.c - system-level PSCI operations (phase 10, plan item 53).
 *
 * Same conduit pattern as kernel/smp.c's static caller: values go
 * out through x0-x3, result lands in x0. QEMU intercepts the hvc at
 * EL1 when the DTB /psci node asked for "hvc"; boards with smc
 * conduit take the other branch (platform.c probed which).
 *
 * SYSTEM_OFF/SYSTEM_RESET are implemented faithfully -- they simply
 * never come back. Everything else here is query plumbing used by
 * the phase-10 selftest to prove conduit reachability without
 * killing the machine.
 */

#include <stdint.h>

#include "lib.h"
#include "panic.h"
#include "platform.h"
#include "psci.h"

static const struct platform_info *plat;

void psci_init(const struct platform_info *pi)
{
    plat = pi;
}

bool psci_available(void)
{
    return plat && plat->has_psci;
}

int64_t psci_call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2)
{
    register uint64_t r0 __asm__("x0") = fn;
    register uint64_t r1 __asm__("x1") = a0;
    register uint64_t r2 __asm__("x2") = a1;
    register uint64_t r3 __asm__("x3") = a2;

    if (!psci_available())
        return PSCI_RET_NOT_SUPPORTED;

    if (plat->psci_hvc)
        __asm__ volatile("hvc #0" : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3)
                         : "memory");
    else
        __asm__ volatile("smc #0" : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3)
                         : "memory");
    return (int64_t)r0;
}

uint32_t psci_version(void)
{
    int64_t r = psci_call(PSCI_FN_VERSION, 0, 0, 0);

    return r < 0 ? 0u : (uint32_t)r;
}

/* FEATURES returns SUCCESS(0) when the fn exists                  */
bool psci_has_feature(uint64_t fn)
{
    return psci_call(PSCI_FN_FEATURES, fn, 0, 0) == PSCI_RET_SUCCESS;
}

void __attribute__((noreturn))
     psci_system_off(void)
{
    if (!psci_available()) {
        panic("system off requested without PSCI conduit");
    }

    kprintf("pm: SYSTEM OFF via PSCI\n");
    psci_call(PSCI_FN_SYSTEM_OFF, 0, 0, 0);
    panic("PSCI SYSTEM_OFF returned");  /* firmware lied              */
}

void __attribute__((noreturn))
     psci_system_reset(void)
{
    if (!psci_available())
        panic("system reset requested without PSCI conduit");

    kprintf("pm: SYSTEM RESET via PSCI\n");
    psci_call(PSCI_FN_SYSTEM_RESET, 0, 0, 0);
    panic("PSCI SYSTEM_RESET returned");
}
