/*
 * selftest.c - phase 2 memory subsystem verification.
 * Runs at boot under QEMU; any failure panics rather than limping on.
 */

#include <stdint.h>
#include <stdbool.h>

#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "panic.h"

static uint32_t lcg_state = 0x12345678u;

static uint32_t lcg(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state >> 8;
}

/* ---- physical allocator ------------------------------------------------ */

static bool test_pmm(void)
{
    struct pmm_stats s;
    paddr_t a;
    uint64_t before;

    pmm_stats_get(&s);
    before = s.free_frames;

    a = pmm_alloc();
    if (!a || !IS_ALIGNED(a, PAGE_SIZE))
        return false;

    pmm_free(a);
    pmm_stats_get(&s);
    return s.free_frames == before;
}

/* ---- virtual mapper ------------------------------------------------------ */

#define TPAGES 4

static bool test_vmm(void)
{
    paddr_t pas[TPAGES];
    unsigned i;

    for (i = 0; i < TPAGES; i++) {
        pas[i] = pmm_alloc();
        if (!pas[i])
            return false;
    }

    /*
     * Write through the higher-half direct map (TTBR1 window, built
     * at boot) and verify the identity view observes the same bytes.
     * Proves both halves of the translation tables route correctly.
     */
    for (i = 0; i < TPAGES; i++) {
        *(uint32_t *)vmm_dmap(pas[i]) = 0xC0DE0000u ^ (i << 4);
    }
    for (i = 0; i < TPAGES; i++) {
        if (*(volatile uint32_t *)pas[i] !=
            (0xC0DE0000u ^ (i << 4)))
            return false;
    }

    /* software walk of the dmap window must agree */
    for (i = 0; i < TPAGES; i++) {
        paddr_t got;

        if (!vmm_translate(vmm_dmap(pas[i]), &got))
            return false;
        if (got != pas[i])
            return false;
    }

    for (i = 0; i < TPAGES; i++)
        pmm_free(pas[i]);

    return true;
}

/* ---- heap stress ---------------------------------------------------------- */

#define NHEAP 400

static bool test_kheap(void)
{
    static uint8_t *p[NHEAP];
    static size_t   sz[NHEAP];
    struct kheap_stats st;
    unsigned i, j, i2;

    for (i = 0; i < NHEAP; i++) {
        sz[i] = (size_t)(lcg() % 3000) + 1;     /* spans slab AND large */
        p[i] = kmalloc(sz[i]);
        for (j = 0; j < sz[i]; j++)
            p[i][j] = (uint8_t)(i ^ sz[i] ^ j);
    }

    for (i = 0; i < NHEAP; i += 2) {
        if (*(volatile uint32_t *)(p[i] - 16) != 0xA110C8EDu)
            kprintf("TRACE: even i=%u ptr=%p magic=%08x\n",
                    i, p[i], *(volatile uint32_t *)(p[i] - 16));
        kprintf("F%u %p\n", i, p[i]);
        kfree(p[i]);
        p[i] = NULL;

        /* pinpoint which free corrupts which chunk */
        for (i2 = 1; i2 < NHEAP; i2 += 2)
            if (p[i2] && *(volatile uint32_t *)(p[i2] - 16) !=
                             0xA110C8EDu) {
                kprintf("CORRUPT: freed i=%u damaged odd i=%u @%p\n",
                        i, i2, p[i2]);
                return false;
            }
    }

    for (i = 1; i < NHEAP; i += 2)              /* odds survive intact  */
        for (j = 0; j < sz[i]; j++)
            if (p[i][j] != (uint8_t)(i ^ sz[i] ^ j))
                return false;

    for (i = 1; i < NHEAP; i += 2) {
        kfree(p[i]);
        p[i] = NULL;
    }

    kheap_stats_get(&st);
    return st.frees == NHEAP && st.bytes_current == 0;
}

/* ---- entry ------------------------------------------------------------------ */

void mem_selftest(void)
{
    struct pmm_stats ps;

    if (!test_pmm())
        panic("selftest: PMM failed");
    kprintf("selftest: pmm .............. ok\n");

    if (!test_vmm())
        panic("selftest: VMM failed");
    kprintf("selftest: vmm .............. ok\n");

    if (!test_kheap())
        panic("selftest: kernel heap failed");
    kprintf("selftest: kheap (%u allocs) . ok\n",
            (unsigned)NHEAP);

    pmm_stats_get(&ps);
    kprintf("mm: %llu/%llu frames free (%llu KiB reserved)\n",
            (unsigned long long)ps.free_frames,
            (unsigned long long)ps.total_frames,
            (unsigned long long)(ps.reserved_frames << (PAGE_SHIFT - 10)));
}
