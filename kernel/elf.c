/*
 * elf.c - ELF64 static loader.
 *
 * All writes go through the identity alias of freshly allocated
 * frames (the loader may run while a DIFFERENT address space is
 * active -- exec builds the new image before switching TTBR0), so
 * no uaccess is involved here: the image lives in kernel .rodata,
 * the destination pages are ours by construction.
 *
 * Segment permission policy: PF_R is implied for every mapped page
 * (a read-only text page still needs instruction fetches and the
 * loader's own copy loop); PF_W and PF_X map straight through to
 * VM_WRITE / VM_EXEC. Everything is VM_USER + non-global so it is
 * ASID-tagged and invisible to kernel threads' probes.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "elf.h"
#include "lib.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc.h"   /* USER_CODE_BASE / USER_VA_LIMIT (the window
                     * check below) -- never rely on a transitive
                     * include for the VA layout contract          */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr;

#define PHDR_ENT_SIZE 56

/* pull one phdr out of the image with bounds checking */
static bool get_phdr(const uint8_t *img, size_t img_len,
                     const elf64_ehdr *eh, unsigned i, elf64_phdr *out)
{
    uint64_t off = eh->e_phoff + (uint64_t)i * eh->e_phentsize;

    if (off + sizeof(*out) > img_len)
        return false;
    memcpy(out, img + off, sizeof(*out));
    return true;
}

static int load_segment(paddr_t root, const uint8_t *img, size_t img_len,
                        const elf64_phdr *ph)
{
    unsigned fl = VM_READ | VM_USER;
    uint64_t va_lo = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE);
    uint64_t va_hi = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
    uint64_t va;

    if (ph->p_flags & PF_W)
        fl |= VM_WRITE;
    if (ph->p_flags & PF_X)
        fl |= VM_EXEC;

    if (ph->p_offset + ph->p_filesz > img_len)
        return ELF_ERR_SEG;
    if (ph->p_vaddr < USER_CODE_BASE ||
        ph->p_vaddr + ph->p_memsz > USER_VA_LIMIT)
        return ELF_ERR_SEG;

    for (va = va_lo; va < va_hi; va += PAGE_SIZE) {
        paddr_t fr = pmm_alloc();

        if (!fr)
            return ELF_ERR_NOMEM;

        if (vmm_map_at(root, va, fr, fl)) {
            pmm_free(fr);
            return ELF_ERR_OVERLAP;
        }

        /*
         * Fill this page from the file where they overlap; zero the
         * rest (covers both head/tail partial pages and .bss).
         */
        {
            uint8_t *pg = (uint8_t *)(uintptr_t)fr;
            uint64_t lo = va > ph->p_vaddr ? va : ph->p_vaddr;
            uint64_t f_end = ph->p_vaddr + ph->p_filesz;
            uint64_t m_end = ph->p_vaddr + ph->p_memsz;
            uint64_t hi_f = va + PAGE_SIZE < f_end ? va + PAGE_SIZE : f_end;
            uint64_t hi_m = va + PAGE_SIZE < m_end ? va + PAGE_SIZE : m_end;

            if (hi_f > lo)
                memcpy(pg + (lo - va),
                       img + ph->p_offset + (lo - ph->p_vaddr),
                       (size_t)(hi_f - lo));
            if (hi_m > hi_f)
                memset(pg + (hi_f - va), 0,
                       (size_t)(hi_m - hi_f));
        }
    }
    return ELF_OK;
}

int elf_load(paddr_t root_pa, const void *img, size_t img_len,
             uint64_t *entry, vaddr_t *brk_base)
{
    const uint8_t *bytes = img;
    elf64_ehdr eh;
    elf64_phdr ph;
    vaddr_t brk = 0;
    unsigned i;

    if (img_len < sizeof(eh))
        return ELF_ERR_HEADER;
    memcpy(&eh, bytes, sizeof(eh));

    if (eh.e_ident[0] != ELF_MAGIC0 || eh.e_ident[1] != ELF_MAGIC1 ||
        eh.e_ident[2] != ELF_MAGIC2 || eh.e_ident[3] != ELF_MAGIC3 ||
        eh.e_ident[4] != ELF_CLASS64 || eh.e_ident[5] != ELF_DATA_LSB)
        return ELF_ERR_HEADER;

    if (eh.e_type != ET_EXEC || eh.e_machine != EM_AARCH64 ||
        eh.e_phentsize != PHDR_ENT_SIZE || eh.e_phnum == 0 ||
        eh.e_phnum > 32)
        return ELF_ERR_TYPE;
    if (eh.e_phoff + (uint64_t)eh.e_phnum * eh.e_phentsize > img_len)
        return ELF_ERR_HEADER;

    /* reject dynamic images up front */
    for (i = 0; i < eh.e_phnum; i++) {
        if (!get_phdr(bytes, img_len, &eh, i, &ph))
            return ELF_ERR_HEADER;
        if (ph.p_type == PT_INTERP)
            return ELF_ERR_DYNAMIC;
    }

    for (i = 0; i < eh.e_phnum; i++) {
        int r;

        if (!get_phdr(bytes, img_len, &eh, i, &ph))
            return ELF_ERR_HEADER;
        if (ph.p_type != PT_LOAD)
            continue;
        if (!ph.p_memsz)
            continue;

        r = load_segment(root_pa, bytes, img_len, &ph);
        if (r)
            return r;

        if ((vaddr_t)ALIGN_UP(ph.p_vaddr + ph.p_memsz, PAGE_SIZE) > brk)
            brk = ALIGN_UP(ph.p_vaddr + ph.p_memsz, PAGE_SIZE);
    }

    if (eh.e_entry < USER_CODE_BASE || eh.e_entry >= USER_VA_LIMIT)
        return ELF_ERR_SEG;

    *entry = eh.e_entry;
    *brk_base = brk;
    return ELF_OK;
}
