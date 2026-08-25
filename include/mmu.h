#ifndef MMU_H
#define MMU_H

#include <stdint.h>

/*
 * Identity-map the kernel's world with a stage-1, 4 KiB-granule,
 * single-level page table using 1 GiB blocks:
 *   - everything below ram_base  -> Device-nGnRE, XN
 *   - ram_base..round_up(size)   -> Normal write-back cacheable
 * Enables MMU + I/D caches on return.
 */
void mmu_enable(uint64_t ram_base, uint64_t ram_size);

/* non-zero once SCTLR_EL1.M is set */
int mmu_active(void);

#endif /* MMU_H */
