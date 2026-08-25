#ifndef MM_KHEAP_H
#define MM_KHEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Kernel heap window inside the TTBR1 higher half.
 * Grows on demand, one page at a time, upward from the base.
 */
#define KERN_HEAP_BASE    0xFFFFA00000000000ULL

struct kheap_stats {
    uint64_t allocs;
    uint64_t frees;
    uint64_t bytes_current;
    uint64_t bytes_peak;
};

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);
void  kheap_stats_get(struct kheap_stats *out);

#endif /* MM_KHEAP_H */
