#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t val;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
    __asm__ volatile("stlr %w0, [%1]" :: "r"(val), "r"(addr) : "memory");
}

#endif /* MMIO_H */
