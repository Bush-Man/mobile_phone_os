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
#include "spinlock.h"

extern uint8_t _start[];
extern uint8_t _end[];

static paddr_t free_head;
static struct pmm_stats stats;
static spinlock_t pmm_lock = SPINLOCK_INIT;

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
    daif_state s;
    paddr_t pa;

    spin_lock_irqsave(&pmm_lock, &s);

    pa = free_head;
    if (!pa) {
        spin_unlock_irqrestore(&pmm_lock, s);
        return 0;
    }

    if (!IS_ALIGNED(pa, PAGE_SIZE)) {
        /* list corrupted: refuse rather than hand out garbage */
        free_head = 0;
        stats.free_frames = 0;
        spin_unlock_irqrestore(&pmm_lock, s);
        return 0;
    }

    free_head = *(paddr_t *)pa;
    stats.free_frames--;

    spin_unlock_irqrestore(&pmm_lock, s);
    return pa;
}

/*
 * Allocate `npages` PHYSICALLY CONTIGUOUS frames. The identity map
 * (VA == PA) makes contiguity a hard requirement for anything that
 * is addressed as one buffer across a page boundary (the kernel
 * heap's multi-page spans).
 *
 * Implementation: single pass over the free list looking for a run
 * of list-consecutive nodes whose addresses are also consecutive.
 * The run is unlinked in O(1) once found (the node before the run
 * adopts the successor of the run's last node), so the whole scan
 * stays O(free frames).
 */
paddr_t pmm_alloc_contig(unsigned npages)
{
    daif_state s;
    paddr_t pa, prev, run_start, run_prev;
    unsigned run;
    int dir;                        /* 0 unknown, +1 up, -1 down */

    if (!npages)
        return 0;

    spin_lock_irqsave(&pmm_lock, &s);

    prev = 0;
    pa = free_head;
    run = 0;
    run_start = 0;
    run_prev = 0;
    dir = 0;

    while (pa) {
        paddr_t next = *(paddr_t *)pa;

        /*
         * Extend the current run. The free list is LIFO: freshly
         * initialized RAM is threaded DESCENDING, and freed spans
         * push descending runs back on top, so both directions
         * occur in practice.
         */
        if (run == 0) {
            run = 1;
            run_start = pa;
            run_prev = prev;
            dir = 0;
        } else if (dir == 0) {
            if (pa == run_start + PAGE_SIZE) {
                dir = 1;
                run = 2;
            } else if (pa == run_start - PAGE_SIZE) {
                dir = -1;
                run = 2;
            } else {
                run = 1;
                run_start = pa;
                run_prev = prev;
            }
        } else if (pa == (dir > 0
                          ? run_start + (paddr_t)run * PAGE_SIZE
                          : run_start - (paddr_t)run * PAGE_SIZE)) {
            run++;
        } else {
            run = 1;
            run_start = pa;
            run_prev = prev;
            dir = 0;
        }

        if (run == npages) {
            /* unlink [run_start .. pa] from the free list */
            if (run_prev)
                *(paddr_t *)run_prev = next;
            else
                free_head = next;
            stats.free_frames -= npages;
            spin_unlock_irqrestore(&pmm_lock, s);
            /* the caller gets the LOW address of the span */
            return dir > 0 ? run_start
                           : run_start - (paddr_t)(npages - 1) * PAGE_SIZE;
        }

        prev = pa;
        pa = next;
    }

    spin_unlock_irqrestore(&pmm_lock, s);
    return 0;
}

void pmm_free(paddr_t pa)
{
    daif_state s;

    if (!pa || !IS_ALIGNED(pa, PAGE_SIZE))
        return;                 /* defensive: ignore bad frees */

    spin_lock_irqsave(&pmm_lock, &s);
    *(paddr_t *)pa = free_head;
    free_head = pa;
    stats.free_frames++;
    spin_unlock_irqrestore(&pmm_lock, s);
}

void pmm_stats_get(struct pmm_stats *out)
{
    *out = stats;
}
