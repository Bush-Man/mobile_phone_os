#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>
#include <stdbool.h>

#include "mm/types.h"

/* permission/attribute flags for vmm_map() */
#define VM_READ   (1u << 0)         /* implied; documented for clarity */
#define VM_WRITE  (1u << 1)
#define VM_EXEC   (1u << 2)
#define VM_USER   (1u << 3)         /* EL0-accessible, non-global      */
#define VM_DEVICE (1u << 4)         /* Device-nGnRE memory             */

/*
 * Kernel higher-half windows (mapped through TTBR1):
 *   physical address X of RAM    -> KERN_DMAP_BASE    + X
 *   physical address X of MMIO   -> KERN_DEVICE_BASE  + X
 * The kernel keeps executing at its identity-mapped link address for
 * now; these windows give every driver/subsystem a stable view of any
 * physical address regardless of where RAM lives.
 */
#define KERN_DMAP_BASE    0xFFFF800000000000ULL
#define KERN_DEVICE_BASE  0xFFFFC00000000000ULL
#define KERN_TEST_BASE    0xFFFF900000000000ULL /* scratch for tests */

static inline vaddr_t vmm_dmap(paddr_t pa)
{
    return KERN_DMAP_BASE + pa;
}

static inline vaddr_t vmm_devmap(paddr_t pa)
{
    return KERN_DEVICE_BASE + pa;
}

struct platform_info;

/* builds PMM free list, installs lower/upper page tables, sets TTBRs */
void vmm_init(const struct platform_info *plat);

/* 4 KiB-page API (intermediate tables allocated on demand) */
int  vmm_map(vaddr_t va, paddr_t pa, unsigned flags);
int  vmm_unmap(vaddr_t va);
bool vmm_translate(vaddr_t va, paddr_t *pa_out);

#endif /* MM_VMM_H */
