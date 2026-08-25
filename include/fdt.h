#ifndef FDT_H
#define FDT_H

#include <stdint.h>

#define FDT_MAGIC 0xd00dfeedU

/* token codes found in the flattened device tree struct block */
enum fdt_token {
    FDT_BEGIN_NODE = 0x1,
    FDT_END_NODE   = 0x2,
    FDT_PROP       = 0x3,
    FDT_NOP        = 0x4,
    FDT_END        = 0x9,
};

/* All header fields are big-endian on the wire; read them via fdt_u32(). */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct fdt {
    const struct fdt_header *hdr;
    const char      *strings;       /* strings block          */
    const uint32_t  *st;            /* struct block, u32 grid */
};

/*
 * Node paths support a trailing '*' wildcard per segment:
 *   "/memory*"       matches /memory@40000000
 *   "/soc/pl011*"    matches /soc/pl011@9000000
 */
int         fdt_init(struct fdt *f, uintptr_t blob);
uint32_t    fdt_u32(const void *p);                 /* big-endian load  */
int         fdt_find_node(const struct fdt *f, const char *path);
const void *fdt_getprop(const struct fdt *f, int node_off,
                        const char *name, int *len_out);

#endif /* FDT_H */
