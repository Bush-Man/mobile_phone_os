/*
 * kheap.c - kernel memory allocator.
 *
 * Design:
 *   - slab-style size classes (16..2048 bytes, power-of-two); each class
 *     has a LIFO freelist threaded through the free chunk bodies
 *   - allocations larger than the top class are served as dedicated,
 *     page-rounded mappings that are unmapped again on free
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
#include "mm/vmm.h"
#include "panic.h"

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

static vaddr_t freelist[KM_CLASSES];
static vaddr_t heap_cursor = KERN_HEAP_BASE;

static struct kheap_stats kstats;

/* ---- internals ---------------------------------------------------------- */

static bool grow_slab(unsigned ci)
{
    paddr_t pa = pmm_alloc();
    vaddr_t va;
    unsigned chunk = HDR_SIZE + class_size[ci];
    unsigned count, i;

    if (!pa)
        return false;

    va = heap_cursor;
    heap_cursor += PAGE_SIZE;
    if (vmm_map(va, pa, VM_READ | VM_WRITE) != 0)
        return false;

    count = PAGE_SIZE / chunk;
    for (i = 0; i < count; i++) {
        struct chunk_hdr *h = (struct chunk_hdr *)(va + i * chunk);

        h->magic  = HDR_MAGIC_FREED;
        h->req    = 0;
        h->cls    = (uint16_t)(ci + 1);
        h->npages = 0;
        *(vaddr_t *)body_of(h) = freelist[ci];
        freelist[ci] = (vaddr_t)body_of(h);
    }
    return true;
}

static void *alloc_large(size_t size)
{
    unsigned pages = (unsigned)(ALIGN_UP(HDR_SIZE + size, PAGE_SIZE) >>
                                PAGE_SHIFT);
    vaddr_t va = heap_cursor;
    unsigned i;

    if (pages > 0xFFFF)         /* fits the header field */
        return NULL;

    for (i = 0; i < pages; i++) {
        paddr_t pa = pmm_alloc();

        if (!pa)
            return NULL;
        if (vmm_map(va + ((vaddr_t)i << PAGE_SHIFT), pa,
                    VM_READ | VM_WRITE) != 0)
            return NULL;
    }

    heap_cursor += (vaddr_t)pages << PAGE_SHIFT;

    {
        struct chunk_hdr *h = (struct chunk_hdr *)va;

        h->magic  = HDR_MAGIC_ALLOC;
        h->req    = (uint32_t)size;
        h->cls    = 0;
        h->npages = (uint16_t)pages;
        *(uint32_t *)((uint8_t *)body_of(h) + size) = CANARY_WORD;
        kstats.allocs++;
        kstats.bytes_current += size;
        if (kstats.bytes_current > kstats.bytes_peak)
            kstats.bytes_peak = kstats.bytes_current;
        return body_of(h);
    }
}

/* ---- public API ----------------------------------------------------------- */

void *kmalloc(size_t size)
{
    unsigned ci, i;

    if (!size)
        size = 1;

    for (ci = 0; ci < KM_CLASSES; ci++)
        if (class_size[ci] >= size)
            break;

    if (ci == KM_CLASSES)
        return alloc_large(size);

    if (!freelist[ci] && !grow_slab(ci))
        return NULL;

    {
        vaddr_t body = freelist[ci];
        struct chunk_hdr *h = hdr_of(body);

        freelist[ci] = *(vaddr_t *)body;
        h->magic = HDR_MAGIC_ALLOC;
        h->req   = (uint32_t)size;
        *(uint32_t *)((uint8_t *)body + size) = CANARY_WORD;

#if KHEAP_DEBUG
        for (i = 0; i < size; i++)
            ((uint8_t *)body)[i] = FILL_BYTE;
#else
        (void)i;
#endif

        kstats.allocs++;
        kstats.bytes_current += size;
        if (kstats.bytes_current > kstats.bytes_peak)
            kstats.bytes_peak = kstats.bytes_current;
        return (void *)body;
    }
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p)
        __builtin_memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    struct chunk_hdr *h;

    if (!ptr)
        panic("kfree(NULL)");

    h = hdr_of(ptr);

    if (h->magic == HDR_MAGIC_FREED)
        panic("kfree: double free");
    if (h->magic != HDR_MAGIC_ALLOC)
        panic("kfree: corrupted chunk header");
    if (*(uint32_t *)((uint8_t *)ptr + h->req) != CANARY_WORD)
        panic("kfree: buffer overrun detected");

#if KHEAP_DEBUG
    __builtin_memset(ptr, POISON_BYTE, h->req);
#endif

    kstats.frees++;
    kstats.bytes_current -= h->req;

    if (h->cls) {
        unsigned ci = h->cls - 1;

        h->magic = HDR_MAGIC_FREED;
        *(vaddr_t *)ptr = freelist[ci];
        freelist[ci] = (vaddr_t)ptr;
        return;
    }

    /* large: unmap and release its frames */
    {
        vaddr_t va = ALIGN_DOWN((vaddr_t)h, PAGE_SIZE);
        unsigned i;

        for (i = 0; i < h->npages; i++) {
            paddr_t pa;

            if (vmm_translate(va + ((vaddr_t)i << PAGE_SHIFT), &pa))
                pmm_free(pa);
            vmm_unmap(va + ((vaddr_t)i << PAGE_SHIFT));
        }
    }
}

void kheap_stats_get(struct kheap_stats *out)
{
    *out = kstats;
}
