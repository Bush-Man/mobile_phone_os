/*
 * vmm.c - stage-1 page tables for AArch64 @ EL1.
 *
 * Two address spaces, split by TCR (T0SZ/T1SZ = 16 -> 48-bit halves):
 *
 *   TTBR0 "lower": kernel context. Identity map of devices + RAM so the
 *                  kernel keeps executing at its link address.
 *   TTBR1 "upper": higher-half windows owned permanently by the kernel
 *                  (direct map of RAM, device window). Later phases add
 *                  per-process user space under TTBR0.
 *
 * All page-table frames come from the PMM and are touched through the
 * identity map (valid while physical RAM lives below 4 GiB -- true for
 * every current target).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lib.h"
#include "mm/pmm.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "panic.h"
#include "platform.h"

/* ---- descriptor encoding -------------------------------------------- */

#define PTE_VALID     (1ull << 0)
#define PTE_TABLE     (1ull << 1)   /* 0b11 = next-level table          */
#define PTE_ATTR(n)   ((n) << 2)
#define ATTR_NORMAL   0ull
#define ATTR_DEVICE   1ull
#define PTE_AP_SHIFT  6
#define PTE_SH_INNER  (3ull << 8)
#define PTE_AF        (1ull << 10)
#define PTE_NG        (1ull << 11)
#define PTE_PXN       (1ull << 53)
#define PTE_UXN       (1ull << 54)

#define PT_ENTRIES    512
#define L0_IDX(va)    (((va) >> 39) & (PT_ENTRIES - 1))
#define L1_IDX(va)    (((va) >> 30) & (PT_ENTRIES - 1))
#define L2_IDX(va)    (((va) >> 21) & (PT_ENTRIES - 1))
#define L3_IDX(va)    (((va) >> 12) & (PT_ENTRIES - 1))

#define GB            GiB(1)
#define VA48_MASK     ((1ull << 48) - 1)

static const uint64_t level_shift[4] = { 39, 30, 21, 12 };

/* ---- static kernel tables (BSS, image-reserved by PMM) --------------- */

static uint64_t lower_l0[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t lower_l1[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t upper_l0[PT_ENTRIES] __attribute__((aligned(4096)));

static uint64_t *root_for(vaddr_t va)
{
    return (va >> 63) ? upper_l0 : lower_l0;
}

/* ---- helpers ---------------------------------------------------------- */

static uint64_t *table_ptr(uint64_t desc)
{
    return (uint64_t *)(desc &
                        ~(VA48_MASK));   /* phys == ident VA here */
}

static uint64_t leaf_desc(paddr_t pa, unsigned flags)
{
    uint64_t ap, xn, attr, ng;

    if (flags & VM_USER)
        ap = (flags & VM_WRITE) ? (1ull << PTE_AP_SHIFT)   /* RW both  */
                                : (3ull << PTE_AP_SHIFT);  /* RO both  */
    else
        ap = (flags & VM_WRITE) ? (0ull << PTE_AP_SHIFT)   /* RW EL1   */
                                : (2ull << PTE_AP_SHIFT);  /* RO EL1   */

    if (!(flags & VM_EXEC))
        xn = PTE_PXN | PTE_UXN;
    else if (flags & VM_USER)
        xn = PTE_PXN;
    else
        xn = PTE_UXN;

    attr = (flags & VM_DEVICE) ? PTE_ATTR(ATTR_DEVICE)
                               : PTE_ATTR(ATTR_NORMAL);
    ng   = (flags & VM_USER) ? PTE_NG : 0;

    return PTE_VALID | attr | ap | PTE_SH_INNER | PTE_AF | ng | xn |
           (pa & ~(PAGE_SIZE - 1));
}

static void tlb_flush_all(void)
{
    __asm__ volatile("tlbi vmalle1is");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}

static void tlb_flush_va(vaddr_t va)
{
    __asm__ volatile("tlbi vaae1is, %0" :: "r"(va));
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}

static uint64_t block_desc(paddr_t pa, unsigned flags)
{
    return leaf_desc(pa, flags) & ~PTE_TABLE;   /* 0b01, never 0b11 */
}

static uint64_t table_desc(paddr_t table_pa)
{
    return PTE_VALID | PTE_TABLE | table_pa;
}

/* ---- public API -------------------------------------------------------- */

int vmm_map(vaddr_t va, paddr_t pa, unsigned flags)
{
    uint64_t *t = root_for(va);
    unsigned lvl;

    if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(pa, PAGE_SIZE))
        return -1;

    for (lvl = 0; lvl < 3; lvl++) {
        unsigned idx = (unsigned)((va >> level_shift[lvl]) &
                                  (PT_ENTRIES - 1));
        uint64_t d = t[idx];

        if (!(d & PTE_VALID)) {
            paddr_t nt = pmm_alloc();

            if (!nt)
                return -2;
            {
                uint64_t *zp = (uint64_t *)nt;

                for (unsigned z = 0; z < PT_ENTRIES; z++)
                    zp[z] = 0;
            }
            t[idx] = table_desc(nt);
            t = table_ptr(t[idx]);
        } else if (d & PTE_TABLE) {
            t = table_ptr(d);
        } else {
            return -3;              /* existing block: refuse to split */
        }
    }

    if (t[L3_IDX(va)] & PTE_VALID)
        return -4;                  /* already mapped */

    t[L3_IDX(va)] = leaf_desc(pa, flags);
    return 0;
}

int vmm_unmap(vaddr_t va)
{
    uint64_t *t = root_for(va);
    unsigned lvl;

    for (lvl = 0; lvl < 3; lvl++) {
        uint64_t d = t[(va >> level_shift[lvl]) & (PT_ENTRIES - 1)];

        if (!(d & PTE_VALID))
            return -1;
        if (!(d & PTE_TABLE)) {     /* unmapping a block: clear it */
            t[(va >> level_shift[lvl]) & (PT_ENTRIES - 1)] = 0;
            tlb_flush_va(va);
            return 0;
        }
        t = table_ptr(d);
    }

    if (!(t[L3_IDX(va)] & PTE_VALID))
        return -1;
    t[L3_IDX(va)] = 0;
    tlb_flush_va(va);
    return 0;
}

bool vmm_translate(vaddr_t va, paddr_t *pa_out)
{
    uint64_t *t = root_for(va);
    unsigned lvl;

    for (lvl = 0; lvl < 3; lvl++) {
        uint64_t d = t[(va >> level_shift[lvl]) & (PT_ENTRIES - 1)];

        if (!(d & PTE_VALID))
            return false;
        if (!(d & PTE_TABLE)) {     /* block */
            uint64_t off = (1ull << level_shift[lvl]) - 1;

            *pa_out = (d & ~(PAGE_SIZE - 1) & VA48_MASK) | (va & off);
            return true;
        }
        t = table_ptr(d);
    }

    {
        uint64_t d = t[L3_IDX(va)];

        if (!(d & PTE_VALID))
            return false;
        *pa_out = (d & ~(PAGE_SIZE - 1) & VA48_MASK) | (va & (PAGE_SIZE - 1));
        return true;
    }
}

/* ---- bring-up ----------------------------------------------------------- */

static void build_lower_identity(paddr_t ram_base, uint64_t ram_size)
{
    paddr_t va;

    lower_l0[L0_IDX(0x40000000UL)] =
        table_desc((paddr_t)(uintptr_t)lower_l1);

    for (va = 0; va < ram_base && va < GiB(4); va += GB)
        lower_l1[L1_IDX(va)] = block_desc(va, VM_READ | VM_WRITE |
                                              VM_DEVICE);
    {
        uint64_t end = ALIGN_UP(ram_base + ram_size, GB);
        unsigned i = 0;

        for (va = ram_base; va < end && i < PT_ENTRIES; va += GB, i++)
            lower_l1[L1_IDX(va)] = block_desc(va, VM_READ | VM_WRITE |
                                                  VM_EXEC);
    }
}

static void build_upper_windows(paddr_t ram_base, uint64_t ram_size)
{
    paddr_t l1_dmap = pmm_alloc();
    paddr_t l1_dev  = pmm_alloc();
    uint64_t end, va;
    unsigned i = 0;

    if (!l1_dmap || !l1_dev)
        panic("vmm: out of frames for kernel windows");

    upper_l0[L0_IDX(KERN_DMAP_BASE)]   = table_desc(l1_dmap);
    upper_l0[L0_IDX(KERN_DEVICE_BASE)] = table_desc(l1_dev);

    end = ALIGN_UP(ram_base + ram_size, GB);
    for (va = ram_base; va < end; va += GB, i++)
        ((uint64_t *)l1_dmap)[i] =
            block_desc(va, VM_READ | VM_WRITE);          /* XN data   */

    ((uint64_t *)l1_dev)[0] =
        block_desc(0, VM_READ | VM_WRITE | VM_DEVICE);   /* PA 0..1G */
}

void vmm_init(const struct platform_info *plat)
{
    uint64_t mair, tcr, sctlr;

    pmm_init(plat->ram_base, plat->ram_size);

    build_lower_identity(plat->ram_base, plat->ram_size);
    build_upper_windows(plat->ram_base, plat->ram_size);

    mair = (0xffull << 0) |         /* idx0: Normal WB, RWA            */
           (0x04ull << 8);          /* idx1: Device-nGnRE              */

    tcr = (16ull << 0)   |          /* T0SZ: lower half is 48-bit      */
          (1ull << 8)    |          /* IRGN0 WB                        */
          (1ull << 10)   |          /* ORGN0 WB                        */
          (3ull << 12)   |          /* SH0 inner                       */
          (16ull << 16)  |          /* T1SZ: upper half is 48-bit      */
          (1ull << 24)   |          /* IRGN1 WB                        */
          (1ull << 26)   |          /* ORGN1 WB                        */
          (3ull << 28)   |          /* SH1 inner                       */
          (2ull << 30);             /* TG1 = 4 KiB                     */

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));
    __asm__ volatile("msr ttbr0_el1, %0" ::
                     "r"((uint64_t)(uintptr_t)lower_l0));
    __asm__ volatile("msr ttbr1_el1, %0" ::
                     "r"((uint64_t)(uintptr_t)upper_l0));
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    /*
     * Transition safety: the old TCR (phase 1) started the lower walk
     * at level 1; writing TTBR0 first makes the new L0 resolve through
     * its table descriptor to exactly the same L1 semantics, and the
     * subsequent T0SZ change keeps them. Identity view never moves.
     */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ull << 12) | (1ull << 2) | (1ull << 0);
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");

    tlb_flush_all();
}
