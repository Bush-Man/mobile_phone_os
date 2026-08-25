/*
 * fdt.c - minimal flattened device tree reader.
 *
 * Supports: header validation, node lookup by wildcard path,
 * property retrieval. Enough for early platform discovery; grows
 * into a fuller API in later phases if needed.
 */

#include <stdint.h>
#include <stddef.h>

#include "fdt.h"

static unsigned long s_len(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    return n;
}

static int s_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int s_ncmp(const char *a, const char *b, unsigned long n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

static const char *s_chr(const char *s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return s;
    return NULL;
}

uint32_t fdt_u32(const void *p)
{
    const uint8_t *b = p;

    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

int fdt_init(struct fdt *f, uintptr_t blob)
{
    f->hdr = (const struct fdt_header *)blob;

    if (fdt_u32(&f->hdr->magic) != FDT_MAGIC)
        return -1;

    f->st      = (const uint32_t *)(blob + fdt_u32(&f->hdr->off_dt_struct));
    f->strings = (const char *)(blob + fdt_u32(&f->hdr->off_dt_strings));
    return 0;
}

/* does 'spec' (one path segment, possibly ending in '*') match 'name'? */
static int seg_match(const char *spec, unsigned long spec_len,
                     const char *name)
{
    if (spec_len && spec[spec_len - 1] == '*')
        return s_ncmp(spec, name, spec_len - 1) == 0;
    return s_ncmp(spec, name, spec_len) == 0 && name[spec_len] == '\0';
}

/* advance *curp past a node's name field */
static const uint32_t *after_name(const uint32_t *tok, const char *name)
{
    return tok + 1 + (s_len(name) + 4) / 4;
}

/* advance *curp past an entire subtree (its BEGIN_NODE already consumed) */
static void skip_subtree(const struct fdt *f, const uint32_t **curp)
{
    int depth = 1;

    (void)f;
    while (depth > 0) {
        switch (fdt_u32(*curp)) {
        case FDT_BEGIN_NODE:
            depth++;
            *curp = after_name(*curp, (const char *)(*curp + 1));
            break;
        case FDT_END_NODE:
            depth--;
            (*curp)++;
            break;
        case FDT_PROP:
            *curp += 3 + (fdt_u32(*curp + 1) + 3) / 4;
            break;

        default:
            (*curp)++;
            break;
        }
    }
}

/* length of the first path segment in 'segs' */
static unsigned long first_seg_len(const char *segs)
{
    const char *slash = s_chr(segs, '/');

    return slash ? (unsigned long)(slash - segs) : s_len(segs);
}

/*
 * Scan tokens inside a node (props + children) looking for the node
 * addressed by remaining path 'segs' ("" means this very node's child
 * list cannot match -- callers never pass "" here).
 * Advances *curp past the terminating FDT_END_NODE.
 * Returns node offset or -1.
 */
static int scan_contents(const struct fdt *f, const uint32_t **curp,
                         const char *segs)
{
    for (;;) {
        uint32_t tok = fdt_u32(*curp);

        switch (tok) {
        case FDT_NOP:
            (*curp)++;
            break;

        case FDT_PROP:
            *curp += 3 + (fdt_u32(*curp + 1) + 3) / 4;
            break;

        case FDT_END_NODE:
            (*curp)++;
            return -1;

        case FDT_BEGIN_NODE: {
            const uint32_t *here = *curp;
            const char *name = (const char *)(here + 1);
            unsigned long seg_len = first_seg_len(segs);
            const uint32_t *after = after_name(here, name);

            if (seg_match(segs, seg_len, name)) {
                const char *rest = segs + seg_len;

                *curp = after;
                if (*rest == '\0')
                    return (int)((const uint8_t *)here -
                                 (const uint8_t *)f->hdr);
                if (*rest == '/')
                    rest++;
                {
                    int r = scan_contents(f, curp, rest);
                    if (r >= 0)
                        return r;
                }
                /* child missed; it was consumed through its END_NODE */
                continue;
            }

            *curp = after;
            skip_subtree(f, curp);
            break;
        }

        default:                        /* FDT_END or garbage */
            return -1;
        }
    }
}

int fdt_find_node(const struct fdt *f, const char *path)
{
    const uint32_t *cur = f->st;
    const char *p = path;

    if (fdt_u32(cur) != FDT_BEGIN_NODE)
        return -1;

    /* root */
    {
        const uint32_t *after = after_name(cur, (const char *)(cur + 1));

        if (s_cmp(p, "/") == 0)
            return (int)((const uint8_t *)cur - (const uint8_t *)f->hdr);
        cur = after;
    }

    while (*p == '/')
        p++;

    return scan_contents(f, &cur, p);
}

const void *fdt_getprop(const struct fdt *f, int node_off,
                        const char *name, int *len_out)
{
    /* node_off is an offset into the whole blob, not the struct block */
    const uint32_t *cur = (const uint32_t *)((const uint8_t *)f->hdr +
                                             node_off);
    const char *node_name = (const char *)(cur + 1);

    if (fdt_u32(cur) != FDT_BEGIN_NODE)
        return NULL;

    for (cur = after_name(cur, node_name);;) {
        switch (fdt_u32(cur)) {
        case FDT_NOP:
            cur++;
            break;

        case FDT_PROP: {
            uint32_t len     = fdt_u32(cur + 1);
            uint32_t nameoff = fdt_u32(cur + 2);
            const char *pname = f->strings + nameoff;

            if (s_cmp(pname, name) == 0) {
                if (len_out)
                    *len_out = (int)len;
                return cur + 3;
            }
            cur += 3 + (len + 3) / 4;
            break;
        }

        case FDT_END_NODE:
            return NULL;

        default:
            return NULL;
        }
    }
}
