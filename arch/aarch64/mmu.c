/*
 * mmu.c - early MMU bring-up for AArch64 at EL1.
 *
 * Strategy: one statically allocated L1 table with 1 GiB block entries.
 * Coarse for now by design -- Phase 2 replaces this with a real page
 * allocator and 4-level mappings; this stage exists to turn caches on
 * safely and give device regions their strong ordering semantics.
 */

#include <stdint.h>

#include "mmu.h"

#define ATTR_NORMAL 0ull            /* MAIR index 0 */
#define ATTR_DEVICE 1ull            /* MAIR index 1 */

/* descriptor bits: block entries at L1/L2 are 0b01 == just VALID;
 * 0b11 would mean a table pointer, descending one level */
#define PTE_VALID    (1ull << 0)
#define PTE_ATTR(n)  ((n) << 2)
#define PTE_SH_INNER (3ull << 8)
#define PTE_AF       (1ull << 10)
#define PTE_PXN      (1ull << 53)
#define PTE_UXN      (1ull << 54)

#define L1_ENTRIES     512
#define BLOCK_SHIFT    30           /* 1 GiB */
#define BLOCK_SIZE     (1ull << BLOCK_SHIFT)
#define VA_INDEX(va)   (((va) >> BLOCK_SHIFT) & (L1_ENTRIES - 1))

static uint64_t l1_table[L1_ENTRIES] __attribute__((aligned(4096)));

static void map_block(uint64_t va, uint64_t pa, uint64_t attr, int pxn)
{
    l1_table[VA_INDEX(va)] = PTE_VALID | PTE_AF |
                             PTE_SH_INNER | PTE_UXN | PTE_ATTR(attr) |
                             (pxn ? PTE_PXN : 0) |
                             (pa & ~(BLOCK_SIZE - 1));
}

void mmu_enable(uint64_t ram_base, uint64_t ram_size)
{
    uint64_t tcr, mair, sctlr;
    uint64_t va;

    /* devices: whole span below RAM (UART, GIC, virtio, ECAM on virt) */
    for (va = 0; va < ram_base && va < (4ull << 30); va += BLOCK_SIZE)
        map_block(va, va, ATTR_DEVICE, 1);

    /* normal RAM, rounded up to the next gigabyte boundary */
    {
        uint64_t end = (ram_base + ram_size + BLOCK_SIZE - 1) &
                       ~(BLOCK_SIZE - 1);

        for (va = ram_base; va < end; va += BLOCK_SIZE)
            map_block(va, va, ATTR_NORMAL, 0);
    }

    mair = (0xffull << 0) |        /* idx 0: inner+outer WB, RW alloc  */
           (0x04ull << 8);         /* idx 1: Device-nGnRE              */

    /* 39-bit VAs both halves -> walk starts at level 1, matching our
     * single L1 table of 1 GiB blocks; 4 KiB granule, WB walk, inner sh */
    tcr = (25ull << 0)   |          /* T0SZ                            */
          (1ull << 8)    |          /* IRGN0 = WB                      */
          (1ull << 10)   |          /* ORGN0 = WB                      */
          (3ull << 12)   |          /* SH0   = inner shareable         */
          (25ull << 16)  |          /* T1SZ                            */
          (1ull << 24)   |          /* IRGN1                           */
          (1ull << 26)   |          /* ORGN1                           */
          (3ull << 28)   |          /* SH1                             */
          (2ull << 30);             /* TG1 = 4 KiB                     */

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l1_table));
    __asm__ volatile("dsb sy");

    __asm__ volatile("ic iallu");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ull << 12) |         /* I: i-cache on */
             (1ull << 2)  |         /* C: d-cache on */
             (1ull << 0);           /* M: mmu on     */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");
}

int mmu_active(void)
{
    uint64_t sctlr;

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (int)(sctlr & 1);
}
