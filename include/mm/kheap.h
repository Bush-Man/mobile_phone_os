#ifndef MM_KHEAP_H
#define MM_KHEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Kernel heap window inside the TTBR1 higher half.
 * Grows on demand, one page at a time, upward from the base.
 * (KERN_HEAP_BASE itself lives in mm/vmm.h.)
 */

struct kheap_stats {
    uint64_t allocs;
    uint64_t frees;
    uint64_t bytes_current;
    uint64_t bytes_peak;
};

#define KM_LARGE_MAX (64u * 1024u)

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);
void  kheap_stats_get(struct kheap_stats *out);

#endif /* MM_KHEAP_H */
