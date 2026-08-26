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
 * Kernel higher-half windows (mapped through TTBR1).
 * TnSZ = 16 -> each half spans 2^48 bytes and walks start at level 0.
 * Windows sit in distinct 512 GiB slots so their L0 entries never
 * collide even when one window spans several gigabytes.
 */
#define KERN_UHALF_BASE   0xFFFF000000000000ULL
#define KERN_DMAP_BASE    (KERN_UHALF_BASE + 0x008000000000ULL) /* +512G */
#define KERN_TEST_BASE    (KERN_UHALF_BASE + 0x010000000000ULL) /* +1T   */
#define KERN_HEAP_BASE    (KERN_UHALF_BASE + 0x020000000000ULL) /* +2T   */
#define KERN_DEVICE_BASE  (KERN_UHALF_BASE + 0x800000000000ULL) /* +8T   */

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

/* ---- per-process address spaces (phase 5) -------------------------------- */

/* root-table management: index 0 of every root is the shared,
 * TLB-global kernel identity subtree; user mappings live under
 * indices [USER_L0_LO, USER_L0_HI) */
paddr_t vmm_kernel_root(void);
paddr_t vmm_shared_l1(void);
paddr_t vmm_root_alloc(void);           /* zeroed root + shared splice */
void    vmm_root_release(paddr_t root, unsigned l0_lo, unsigned l0_hi);
void    vmm_root_free(paddr_t root);

unsigned vmm_decode_flags(uint64_t desc);
bool    vmm_probe(paddr_t root, vaddr_t va, paddr_t *pa_out,
                  unsigned *flags_out);
int     vmm_map_at(paddr_t root, vaddr_t va, paddr_t pa, unsigned flags);
int     vmm_unmap_at(paddr_t root, vaddr_t va);

/* fork(): deep-copy user pages of [l0_lo, l0_hi) from src into dst */
int     vmm_copy_space(paddr_t dst_root, paddr_t src_root,
                       unsigned l0_lo, unsigned l0_hi);

/* SMP: apply the boot cpu's MMU config on a secondary; clean the
 * kernel image to DRAM so cache-cold secondaries can start */
void vmm_cpu_activate(void);
void vmm_sync_kernel_to_ram(void);

#endif /* MM_VMM_H */
