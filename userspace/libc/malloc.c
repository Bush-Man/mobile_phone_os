/*
 * malloc.c - brk-backed allocator (phase 14).
 *
 * Growth comes from sbrk(), which drives SYS_brk: the kernel maps
 * zeroed user pages above the image and refuses to shrink below
 * brk_floor, so the arena stays sane for the process's lifetime.
 *
 * Layout: every block carries an 8-byte header {size|LSB-used} and
 * lives on one singly-linked free list when released. Coalescing is
 * immediate-neighbor only (prev scan on free) -- simple and plenty
 * for the shell/daemons' allocation patterns.
 */

#include "libc.h"

struct blk {
    u64 size;                   /* payload bytes; bit0 = in use      */
    struct blk *next;           /* free-list link (valid when free)  */
};

#define HDR       (sizeof(struct blk))
#define ALIGN8(x) (((x) + 7u) & ~(u64)7u)

static struct blk *freelist;
static char *heap_top;          /* first byte never handed out       */
static char *heap_end;          /* brk frontier                      */

/* extend the arena by incr bytes; returns old frontier or NULL.
 * incr == 0 just initializes/queries and reports the current top  */
static char *morecore(i64 incr)
{
    char *old;
    void *r;

    if (!heap_top) {
        /* first call: brk(0) reports the loader-set image top     */
        heap_top = (char *)_sys1(SYS_brk, 0);
        heap_end = heap_top;
        if (!incr)
            return heap_top;
    }

    old = heap_end;
    r = (void *)_sys1(SYS_brk, (i64)heap_end + incr);
    if (r == heap_end)            /* kernel refuses => unchanged    */
        return NULL;
    heap_end = (char *)r;
    return old;
}

void *malloc(size_t n)
{
    struct blk *b, **prev;
    u64 need;

    if (!n)
        return NULL;
    need = ALIGN8(n);

    /* first fit over the free list */
    for (prev = &freelist, b = freelist; b; prev = &b->next, b = b->next) {
        if (b->size >= need) {
            *prev = b->next;
            b->size |= 1u;
            return (void *)((char *)b + HDR);
        }
    }

    if (!heap_end && !morecore(0))
        return NULL;

    {
        i64 chunk = (i64)(need + HDR) < 4096
                        ? 4096
                        : (i64)ALIGN8(need + HDR);
        char *base = morecore(chunk);

        if (!base)
            return NULL;

        b = (struct blk *)base;
        b->size = (u64)chunk - HDR;
        b->next = NULL;

        /* return the front, park the tail on the free list        */
        if (b->size > need + HDR + 16u) {
            struct blk *tail =
                (struct blk *)(base + HDR + need);

            tail->size = b->size - need - HDR;
            tail->next = freelist;
            freelist = tail;
            b->size = need;
        }
        b->size |= 1u;
        return (void *)((char *)b + HDR);
    }
}

void free(void *p)
{
    struct blk *b;

    if (!p)
        return;
    b = (struct blk *)((char *)p - HDR);
    b->size &= ~(u64)1u;
    b->next = freelist;
    freelist = b;
}

void *calloc(size_t nm, size_t sz)
{
    void *p = malloc(nm * sz);

    if (p)
        memset(p, 0, nm * sz);
    return p;
}

void *realloc(void *p, size_t n)
{
    struct blk *b;
    void *np;
    u64 old;

    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    b = (struct blk *)((char *)p - HDR);
    old = b->size & ~(u64)1u;
    if (old >= n)
        return p;

    np = malloc(n);
    if (!np)
        return NULL;
    memcpy(np, p, old);
    free(p);
    return np;
}

/* sbrk: raw bump allocator over the same SYS_brk window; exposed
 * for tests that want allocator-free arena behavior                */
char *sbrk(i64 incr)
{
    char *old;

    if (!heap_end) {
        heap_top = (char *)_sys1(SYS_brk, 0);
        heap_end = heap_top;
    }
    old = heap_end;
    if (incr) {
        void *r = (void *)_sys1(SYS_brk, (i64)heap_end + incr);

        if (r == heap_end)
            return (void *)-1;
        heap_end = (char *)r;
    }
    return old;
}
