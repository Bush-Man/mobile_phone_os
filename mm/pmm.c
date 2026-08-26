/*
 * pmm.c - physical frame allocator.
 *
 * LIFO free list threaded through the first word of the free frames
 * themselves (zero metadata). Frames are PAGE_SIZE.
 */

#include <stdint.h>
#include <stddef.h>

#include "lib.h"
#include "mm/pmm.h"
#include "mm/types.h"

extern uint8_t _start[];
extern uint8_t _end[];

static paddr_t free_head;
static struct pmm_stats stats;

void pmm_init(paddr_t ram_base, uint64_t ram_size)
{
    paddr_t img_start = ALIGN_DOWN((paddr_t)_start, PAGE_SIZE);
    paddr_t img_end   = ALIGN_UP((paddr_t)_end, PAGE_SIZE);
    paddr_t pa, ram_end;

    /*
     * Guard the top of RAM: when QEMU boots a kernel directly it
     * places its generated DTB / loader payload at the top of guest
     * memory, and real firmware does the same. Never hand those
     * frames out.
     */
    const uint64_t TOP_GUARD = MiB(2);

    free_head = 0;
    stats.total_frames = 0;
    stats.reserved_frames = 0;
    stats.free_frames = 0;

    if (!ram_size)
        return;

    ram_end = ram_base + ram_size;
    if (ram_end > TOP_GUARD)
        ram_end -= TOP_GUARD;

    for (pa = ALIGN_UP(ram_base, PAGE_SIZE); pa < ram_end; pa += PAGE_SIZE) {
        if (pa >= img_start && pa < img_end) {
            stats.reserved_frames++;
            continue;
        }
        *(paddr_t *)pa = free_head;     /* identity-mapped at boot */
        free_head = pa;
        stats.total_frames++;
        stats.free_frames++;
    }
}

paddr_t pmm_alloc(void)
{
    paddr_t pa = free_head;

    if (!pa)
        return 0;

    if (!IS_ALIGNED(pa, PAGE_SIZE)) {
        /* list corrupted: refuse rather than hand out garbage */
        free_head = 0;
        stats.free_frames = 0;
        return 0;
    }

    free_head = *(paddr_t *)pa;
    stats.free_frames--;
    return pa;
}

void pmm_free(paddr_t pa)
{
    if (!pa || !IS_ALIGNED(pa, PAGE_SIZE))
        return;                 /* defensive: ignore bad frees */
    *(paddr_t *)pa = free_head;
    free_head = pa;
    stats.free_frames++;
}

void pmm_stats_get(struct pmm_stats *out)
{
    *out = stats;
}
