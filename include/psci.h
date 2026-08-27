#ifndef PSCI_H
#define PSCI_H

#include <stdbool.h>
#include <stdint.h>

struct platform_info;

/*
 * PSCI power-management interface (phase 10, plan item 53).
 *
 * kernel/psci.c keeps its own minimal conduit caller -- kernel/smp.c
 * had the same five lines static, and sharing would have meant moving
 * smp_plat ownership; deliberate duplication with cross-reference,
 * consistent with how fat32/ext2 carried their fs locks pre-phase-8.
 *
 * Function-id constants follow SMC Calling Convention numbering;
 * the CONDUIT itself comes from the DTB /psci node (platform.c),
 * exactly like CPU_ON during SMP bring-up.
 */

/* SMC32 ids (system-level, always available)                     */
#define PSCI_FN_VERSION          0x84000000ull
#define PSCI_FN_CPU_SUSPEND      0x84000001ull
#define PSCI_FN_CPU_OFF          0x84000002ull
#define PSCI_FN_CPU_ON           0x84000003ull
#define PSCI_FN_AFFINITY_INFO    0x84000004u
#define PSCI_FN_SYSTEM_OFF       0x84000008u
#define PSCI_FN_SYSTEM_RESET     0x84000009u
#define PSCI_FN_FEATURES         0x8400000au

/* SMC64 variants requested via FEATURES when supported           */
#define PSSI_FN64_CPU_SUSPEND    0xc4000001ull

/* return codes                                                    */
#define PSCI_RET_SUCCESS            0
#define PSCI_RET_NOT_SUPPORTED     (-1)
#define PSCI_RET_INVALID_PARAMS    (-2)
#define PSCI_RET_DENIED            (-3)
#define PSCI_RET_ALREADY_ON        (-4)
#define PSCI_RET_ON_PENDING        (-5)
#define PSCI_RET_INTERNAL_FAILURE  (-6)

/* ---- lifecycle -------------------------------------------------------------------- */

/* install the platform snapshot taken during early bring-up       */
void psci_init(const struct platform_info *plat);
bool psci_available(void);

/* raw conduit passthrough (fn,a0,a1,a2 -> x0)                      */
int64_t psci_call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2);

/* ---- queries ------------------------------------------------------------------------ */

uint32_t psci_version(void);            /* 0 when absent              */
bool     psci_has_feature(uint64_t fn); /* PSCI_FEATURES probe        */

/* ---- destructive system operations ----------------------------------------------- */

/*
 * NOTE THE HAZARD: these two never return (the machine ends there).
 * They exist for the battery-shutdown funnel and diagnostics; the
 * mock-battery CI path deliberately never reaches them -- see
 * drivers/battery.c.
 */
void __attribute__((noreturn))
     psci_system_off(void);

void __attribute__((noreturn))
     psci_system_reset(void);

#endif /* PSCI_H */