/*
 * uaccess.c - fault-tolerant, validated access to user memory.
 *
 * Validation model: because the kernel runs with the process's
 * TTBR0 active but touches user data through the identity alias,
 * every page of a range is software-walked (vmm_probe) BEFORE the
 * copy. A page qualifies only if
 *
 *   - its leaf descriptor exists and is EL0-accessible (VM_USER),
 *   - it carries the permission the direction needs (VM_WRITE for
 *     copy-out; reads are implied by a valid user mapping),
 *   - the range stays inside [.., USER_VA_LIMIT) without wrapping.
 *
 * With those checks done the physical copies cannot abort, so no
 * fixup-table machinery is needed on this path. Anything that
 * somehow still faults at EL0 is handled as a SIGSEGV in
 * proc_user_fault() -- belt and braces.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lib.h"
#include "mm/vmm.h"
#include "panic.h"
#include "proc.h"
#include "uaccess.h"

static bool range_ok(uint64_t va, size_t len)
{
    uint64_t end;

    if (va >= USER_VA_LIMIT)
        return false;
    if (!len)
        return true;
    end = va + len - 1;
    if (end < va)                       /* wrapped */
        return false;
    return end < USER_VA_LIMIT;
}

/* validate every page of [va, va+len): returns 0 or -EFAULT */
static int check_range(paddr_t root, uint64_t va, size_t len, bool write)
{
    while (len) {
        unsigned fl;
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));

        if (chunk > len)
            chunk = len;

        if (!vmm_probe(root, ALIGN_DOWN(va, PAGE_SIZE), NULL, &fl))
            return -EFAULT;             /* unmapped */
        if (!(fl & VM_USER))
            return -EFAULT;             /* kernel-only alias: refuse */
        if (write && !(fl & VM_WRITE))
            return -EFAULT;
        /* device pages are allowed: drivers will share buffers */

        va += chunk;
        len -= chunk;
    }
    return 0;
}

static long copy_in(void *kdst, paddr_t root, uint64_t usrc, size_t len)
{
    if (!range_ok(usrc, len) || check_range(root, usrc, len, false))
        return -EFAULT;

    while (len) {
        paddr_t pa;
        size_t chunk = PAGE_SIZE - (usrc & (PAGE_SIZE - 1));
        const char *src_va;

        if (chunk > len)
            chunk = len;
        vmm_probe(root, usrc, &pa, NULL);
        src_va = (const char *)(uintptr_t)(pa & ~(uint64_t)(PAGE_SIZE - 1));
        memcpy(kdst, src_va + (usrc & (PAGE_SIZE - 1)), chunk);
        kdst = (char *)kdst + chunk;
        usrc += chunk;
        len -= chunk;
    }
    return 0;
}

static long copy_out(paddr_t root, uint64_t udst, const void *ksrc,
                     size_t len)
{
    if (!range_ok(udst, len) || check_range(root, udst, len, true))
        return -EFAULT;

    while (len) {
        paddr_t pa;
        size_t chunk = PAGE_SIZE - (udst & (PAGE_SIZE - 1));
        char *dst_va;

        if (chunk > len)
            chunk = len;
        vmm_probe(root, udst, &pa, NULL);
        dst_va = (char *)(uintptr_t)(pa & ~(uint64_t)(PAGE_SIZE - 1));
        memcpy(dst_va + (udst & (PAGE_SIZE - 1)), ksrc, chunk);
        ksrc = (const char *)ksrc + chunk;
        udst += chunk;
        len -= chunk;
    }
    return 0;
}

/* ---- public API ----------------------------------------------------------- */

long uacc_copy_in(void *kdst, paddr_t root, uint64_t usrc, size_t len)
{
    return copy_in(kdst, root, usrc, len);
}

long uacc_copy_out(paddr_t root, uint64_t udst, const void *ksrc,
                   size_t len)
{
    return copy_out(root, udst, ksrc, len);
}

long uacc_strnlen_user(paddr_t root, uint64_t usrc, size_t max)
{
    size_t n = 0;

    while (n < max) {
        paddr_t pa;
        char c;

        if (!range_ok(usrc + n, 1) ||
            check_range(root, usrc + n, 1, false))
            return -EFAULT;
        vmm_probe(root, usrc + n, &pa, NULL);
        c = *(volatile const char *)(uintptr_t)pa;
        if (!c)
            return (long)n;
        n++;
    }
    return -EFAULT;                     /* ran off the cap */
}

long uacc_copy_in_cur(void *kdst, const void *usrc, size_t len)
{
    struct proc *p = proc_current();

    if (!p)
        panic("uaccess outside a process");
    return copy_in(kdst, p->root_pa, (uint64_t)(uintptr_t)usrc, len);
}

long uacc_copy_out_cur(void *udst, const void *ksrc, size_t len)
{
    struct proc *p = proc_current();

    if (!p)
        panic("uaccess outside a process");
    return copy_out(p->root_pa, (uint64_t)(uintptr_t)udst, ksrc, len);
}

long uacc_strnlen_user_cur(const void *usrc, size_t max)
{
    struct proc *p = proc_current();

    if (!p)
        panic("uaccess outside a process");
    return uacc_strnlen_user(p->root_pa, (uint64_t)(uintptr_t)usrc, max);
}
