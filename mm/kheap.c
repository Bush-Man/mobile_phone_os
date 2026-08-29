/*
 * kheap.c - kernel memory allocator.
 *
 * Design:
 *   - memory comes straight from the PMM and is used through the
 *     identity map (VA == PA for all RAM below 4 GiB -- true for every
 *     current target); no higher-half window is required
 *   - slab-style size classes (16..2048 bytes, power-of-two); each
 *     class has a LIFO freelist threaded through the free chunk bodies
 *   - allocations larger than the top class are served as dedicated
 *     multi-page spans; freed spans return their pages to the PMM
 *   - every chunk carries a header (magic / request size / class /
 *     page count) plus an end canary written just past the requested
 *     span, verified on free
 *
 * KHEAP_DEBUG adds fill patterns: fresh allocations read 0xAA, freed
 * memory is poisoned with 0x5A, making use-after-free and uninit-use
 * visible immediately instead of silently corrupting state.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/types.h"
#include "panic.h"
#include "spinlock.h"

#ifndef KHEAP_DEBUG
#define KHEAP_DEBUG 1
#endif

#define HDR_MAGIC_ALLOC 0xA110C8EDu
#define HDR_MAGIC_FREED 0xF7EEB1DEu
#define CANARY_WORD     0xCAFEF00Du
#define POISON_BYTE     0x5A
#define FILL_BYTE       0xAA

struct chunk_hdr {
    uint32_t magic;
    uint32_t req;               /* requested size                    */
    uint16_t cls;               /* slab class index + 1; 0 = large   */
    uint16_t npages;            /* large allocations only            */
};
/* header sits 16 bytes ahead of the returned pointer */
#define HDR_SIZE      16u
#define hdr_of(p)     ((struct chunk_hdr *)((uint8_t *)(p) - HDR_SIZE))
#define body_of(h)    ((void *)((uint8_t *)(h) + HDR_SIZE))

#define KM_CLASSES   8
static const uint16_t class_size[KM_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* freelists 0..KM_CLASSES-1: slab classes; KM_CLASSES: large spans */
static vaddr_t freelist[KM_CLASSES + 1];

/*
 * The allocator state (freelists, chunk headers, stats, the recent
 * ring) is shared by every cpu: kmalloc/kfree must serialize, and
 * IRQs must be masked while the lock is held (timer/tasklet paths
 * allocate too).
 */
static spinlock_t kheap_lock = SPINLOCK_INIT;

static struct kheap_stats kstats;

/* post-mortem ring for allocator debugging */
static struct { void *ptr; unsigned sz; } recent[16];
static unsigned recent_idx;

/* ---- internals ---------------------------------------------------------- */

static bool grow_slab(unsigned ci)
{
    paddr_t pa = pmm_alloc();
    unsigned chunk = HDR_SIZE + class_size[ci];
    unsigned count = PAGE_SIZE / chunk;
    unsigned i;

    if (!pa)
        return false;

    /* carve the fresh frame into same-class chunks (LIFO order) */
    for (i = count; i > 0; i--) {
        struct chunk_hdr *h =
            (struct chunk_hdr *)(pa + (i - 1) * chunk);

        h->magic  = HDR_MAGIC_FREED;
        h->req    = 0;
        h->cls    = (uint16_t)(ci + 1);
        h->npages = 0;
        *(vaddr_t *)body_of(h) = freelist[ci];
        freelist[ci] = (vaddr_t)body_of(h);
    }
    return true;
}

/* ---- public API ----------------------------------------------------------- */

void *kmalloc(size_t size)
{
    unsigned ci, i;
    void *body;
    daif_state s;

    if (!size)
        size = 1;

    for (ci = 0; ci < KM_CLASSES; ci++)
        if (size + 4 <= (unsigned)class_size[ci])
            break;

    spin_lock_irqsave(&kheap_lock, &s);

    if (ci == KM_CLASSES) {
        /* large path: dedicated span, pages tracked for free() */
        unsigned pages = (unsigned)((HDR_SIZE + size + PAGE_SIZE - 1) >>
                                    PAGE_SHIFT);
        paddr_t span;

        if (pages > KM_LARGE_MAX / PAGE_SIZE) {
            spin_unlock_irqrestore(&kheap_lock, s);
            return NULL;
        }

        /*
         * One PHYSICALLY CONTIGUOUS span: the identity map (VA==PA)
         * means the caller addresses the whole allocation as a
         * single buffer, so page-by-page pmm_alloc() would scatter
         * it across unrelated frames and corrupt whoever owns them.
         */
        span = pmm_alloc_contig(pages);
        if (!span) {
            spin_unlock_irqrestore(&kheap_lock, s);
            return NULL;
        }

        /* header lives INSIDE the span, at its very start */
        body = (void *)((uint8_t *)span + HDR_SIZE);
        {
            struct chunk_hdr *h = hdr_of(body);

            h->magic  = HDR_MAGIC_ALLOC;
            h->req    = (uint32_t)size;
            h->cls    = 0;
            h->npages = (uint16_t)pages;
        }

        /* canary only when 4 spare bytes remain inside the span */
        if (((size + HDR_SIZE) % PAGE_SIZE) <= PAGE_SIZE - 4)
            *(uint32_t *)((uint8_t *)body + size) = CANARY_WORD;
#if KHEAP_DEBUG
        memset(body, FILL_BYTE, size);
#endif
        kstats.allocs++;
        kstats.bytes_current += size;
        if (kstats.bytes_current > kstats.bytes_peak)
            kstats.bytes_peak = kstats.bytes_current;
        spin_unlock_irqrestore(&kheap_lock, s);
        return body;
    }

    if (!freelist[ci] && !grow_slab(ci)) {
        spin_unlock_irqrestore(&kheap_lock, s);
        return NULL;
    }

    body = (void *)freelist[ci];
    freelist[ci] = *(vaddr_t *)body;

    {
        struct chunk_hdr *h = hdr_of(body);

        h->magic = HDR_MAGIC_ALLOC;
        h->req   = (uint32_t)size;

        /* canary needs spare room inside the chunk */
        if (size < class_size[ci])
            *(uint32_t *)((uint8_t *)body + size) = CANARY_WORD;
    }

#if KHEAP_DEBUG
    for (i = 0; i < size; i++)
        ((uint8_t *)body)[i] = FILL_BYTE;
#else
    (void)i;
#endif

    recent[recent_idx].ptr = body;
    recent[recent_idx].sz  = (unsigned)size;
    recent_idx = (recent_idx + 1) % 16;

    kstats.allocs++;
    kstats.bytes_current += size;
    if (kstats.bytes_current > kstats.bytes_peak)
        kstats.bytes_peak = kstats.bytes_current;

    spin_unlock_irqrestore(&kheap_lock, s);
    return body;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    struct chunk_hdr *h;
    daif_state s;

    if (!ptr)
        panic("kfree(NULL)");

    spin_lock_irqsave(&kheap_lock, &s);

    h = hdr_of(ptr);

    if (h->magic == HDR_MAGIC_FREED)
        panic("kfree: double free");
    if (h->magic != HDR_MAGIC_ALLOC) {
        unsigned z;

        kprintf("BADHDR ptr=%p magic=%08x req=%u cls=%u np=%u\n",
                ptr, h->magic, h->req, h->cls, h->npages);
        kprintf("last allocs:\n");
        for (z = 0; z < 16; z++) {
            unsigned idx = (recent_idx + 15 - z) % 16;

            kprintf("  %p sz=%u\n", recent[idx].ptr, recent[idx].sz);
        }
        panic("kfree: corrupted chunk header");
    }
    {
        int have_canary = h->cls
            ? (h->req + 4 <= (unsigned)class_size[h->cls - 1])
            : ((h->req + HDR_SIZE) % PAGE_SIZE <=
               PAGE_SIZE - 4);

        if (have_canary &&
            *(uint32_t *)((uint8_t *)ptr + h->req) != CANARY_WORD) {
            kprintf("OVERRUN: req=%u cls=%u found=%08x\n",
                    h->req, h->cls,
                    *(uint32_t *)((uint8_t *)ptr + h->req));
            panic("kfree: buffer overrun detected");
        }
    }

#if KHEAP_DEBUG
    memset(ptr, POISON_BYTE, h->req);
#endif

    kstats.frees++;
    kstats.bytes_current -= h->req;

    if (h->cls) {
        /* slab chunk: recycle into its class freelist */
        unsigned ci = h->cls - 1;

        h->magic = HDR_MAGIC_FREED;
        *(vaddr_t *)ptr = freelist[ci];
        freelist[ci] = (vaddr_t)ptr;
        spin_unlock_irqrestore(&kheap_lock, s);
        return;
    }

    /* large span: release its pages back to the PMM */
    {
        paddr_t pa = (paddr_t)(uintptr_t)hdr_of(ptr);
        uint16_t pg;

        for (pg = 0; pg < h->npages; pg++)
            pmm_free(pa + ((paddr_t)pg << PAGE_SHIFT));
    }

    spin_unlock_irqrestore(&kheap_lock, s);
}

void kheap_stats_get(struct kheap_stats *out)
{
    *out = kstats;
}
