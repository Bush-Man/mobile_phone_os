#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>
#include "mm/types.h"

struct pmm_stats {
    uint64_t total_frames;      /* usable frames discovered        */
    uint64_t reserved_frames;   /* kernel image and friends        */
    uint64_t free_frames;       /* currently on the free list      */
};

/*
 * Build the free list from a RAM range, skipping the kernel image.
 * The free list lives in the free frames themselves: each frame's
 * first word holds the next free frame's physical address.
 */
void         pmm_init(paddr_t ram_base, uint64_t ram_size);
paddr_t      pmm_alloc(void);
void         pmm_free(paddr_t pa);
void         pmm_stats_get(struct pmm_stats *out);

#endif /* MM_PMM_H */
