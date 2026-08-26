#ifndef ELF_H
#define ELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/types.h"

/*
 * Minimal ELF64 program-loader (plan item 29).
 *
 * Loads static ET_EXEC AArch64 images into a prepared address-space
 * root: every PT_LOAD segment gets fresh pages mapped with the
 * segment's p_flags, file bytes are copied in and the .bss tail
 * (p_memsz > p_filesz) is zeroed. The caller receives the entry PC
 * and the first free byte above the highest segment (heap base).
 *
 * Dynamic-linked images are rejected (PT_INTERP => -ENOEXEC); the
 * layout produced here (fixed link addresses, plain LOADs, auxv on
 * the stack) is deliberately dynamic-linker-ready.
 */

/* ELF64 constants we care about */
#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELF_CLASS64 2
#define ELF_DATA_LSB 1
#define ET_EXEC 2
#define EM_AARCH64 183

#define PT_LOAD 1
#define PT_INTERP 3

#define PF_X 1
#define PF_W 2
#define PF_R 4

/* errors */
#define ELF_OK           0
#define ELF_ERR_HEADER  -1       /* truncated / bad ident / class   */
#define ELF_ERR_TYPE    -2       /* not ET_EXEC AArch64             */
#define ELF_ERR_SEG     -3       /* PT_LOAD out of bounds           */
#define ELF_ERR_OVERLAP -4       /* segments collide                */
#define ELF_ERR_NOMEM   -5
#define ELF_ERR_DYNAMIC -6       /* PT_INTERP present               */

/*
 * Load `img` (a complete ELF file image, length img_len) into the
 * address space rooted at root_pa (must be a fresh vmm_root_alloc()
 * table). On success returns ELF_OK, sets *entry and *brk_base.
 */
int elf_load(paddr_t root_pa, const void *img, size_t img_len,
             uint64_t *entry, vaddr_t *brk_base);

#endif /* ELF_H */
