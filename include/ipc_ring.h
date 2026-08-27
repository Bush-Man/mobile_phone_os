#ifndef IPC_RING_H
#define IPC_RING_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Byte ring shared by the phase-8 IPC transports (pipes in kernel/
 * ipc.c, stream endpoints in kernel/unixsock.c). Single-producer /
 * single-consumer by construction -- callers serialize access with
 * their subsystem lock -- so head/len bookkeeping is kept minimal.
 * Data is stored in <=2 contiguous segments; every routine makes
 * progress bounded by one memcpy, which lets transport code decide
 * partial-read/write policy without ring knowledge.
 */

struct ipc_ring {
    uint8_t  *buf;
    unsigned  cap;
    unsigned  head;                 /* consumer index               */
    unsigned  len;
};

/* bytes immediately pullable / pushable */
static inline unsigned ipc_ring_used(const struct ipc_ring *r)
{
    return r->len;
}

static inline unsigned ipc_ring_free(const struct ipc_ring *r)
{
    return r->cap - r->len;
}

/* pull up to n bytes; returns how many were moved */
static inline size_t ipc_ring_pull(struct ipc_ring *r,
                                   void *dst, size_t n)
{
    size_t done = 0;

    if (n > r->len)
        n = r->len;
    while (done < n) {
        unsigned seg = r->cap - r->head;

        if ((size_t)seg > n - done)
            seg = (unsigned)(n - done);
        memcpy((uint8_t *)dst + done, &r->buf[r->head], seg);
        r->head = (r->head + seg) % r->cap;
        done += seg;
    }
    r->len -= (unsigned)done;
    return done;
}

/* push up to n bytes; returns how many fit */
static inline size_t ipc_ring_push(struct ipc_ring *r,
                                   const void *src, size_t n)
{
    size_t done = 0;

    if (n > ipc_ring_free(r))
        n = ipc_ring_free(r);
    while (done < n) {
        unsigned tail = (r->head + r->len) % r->cap;
        unsigned seg = r->cap - tail;

        if ((size_t)seg > n - done)
            seg = (unsigned)(n - done);
        memcpy(&r->buf[tail], (const uint8_t *)src + done, seg);
        r->len += seg;
        done += seg;
    }
    return done;
}

#endif /* IPC_RING_H */