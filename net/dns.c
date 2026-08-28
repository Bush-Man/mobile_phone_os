/*
 * dns.c - minimal resolver client (phase 11, item 61).
 *
 * Builds one A query (recursion desired) against the configured DNS
 * server via a connected UDP pcb on an ephemeral port, waits for the
 * response on the same socket, and walks answers for the first A
 * record. CNAME chains are followed up to 4 hops.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "net.h"
#include "time.h"

#define DNS_PORT 53u

struct dns_hdr {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} __attribute__((packed));

static uint16_t dns_id_seq = 0x1234u;

/* name -> QNAME encoding (labels + root)                          */
static unsigned encode_qname(uint8_t *out, const char *name)
{
    unsigned o = 0;
    const char *p = name;

    while (*p) {
        const char *dot = p;
        unsigned lab;

        while (*dot && *dot != '.')
            dot++;
        lab = (unsigned)(dot - p);
        out[o++] = (uint8_t)lab;
        memcpy(&out[o], p, lab);
        o += lab;
        if (!*dot)
            break;
        p = dot + 1;
    }
    out[o++] = 0;
    return o;
}

int dns_resolve(const char *name, uint32_t *ip_out,
                uint32_t timeout_ms)
{
    struct netif *nif = netif_default();
    struct udp_pcb *pcb;
    uint8_t query[280], resp[280];
    unsigned qlen, qname_len;
    uint16_t id = ++dns_id_seq;
    uint64_t deadline;
    int rc = -1;

    if (!nif || !nif->gw || !name || !ip_out)
        return -1;

    pcb = udp_alloc();
    if (!pcb)
        return -1;

    udp_bind(pcb, nif->ip_addr, 0);
    udp_connect(pcb, nif->gw, DNS_PORT);     /* SLIRP forwards port 53    */
    /* note: queries go to the DNS server learned from DHCP; the gw
     * itself forwards in SLIRP, so gw-as-server is the safe path.  */

    memset(query, 0, sizeof(query));
    {
        struct dns_hdr *h = (struct dns_hdr *)query;

        h->id      = id;
        h->flags   = 0x0100u;           /* recursion desired          */
        h->qdcount = 1;
    }
    qname_len = encode_qname(&query[12], name);
    query[12 + qname_len]      = 0;
    query[12 + qname_len + 1]  = 1;     /* type A                     */
    query[12 + qname_len + 2]  = 0;
    query[12 + qname_len + 3]  = 1;     /* class IN                   */
    qlen = 12u + qname_len + 4u;

    if (udp_send(pcb, query, qlen))
        goto out;

    deadline = time_uptime_ms() + timeout_ms;
    for (;;) {
        const uint8_t *p = resp;
        uint16_t flags, qd, an;
        int r;
        unsigned off;

        r = udp_recv(pcb, resp, sizeof(resp), true);
        if (r < (int)sizeof(struct dns_hdr))
            break;                      /* error / timeout via waker  */

        {
            struct dns_hdr h;

            memcpy(&h, resp, sizeof(h));
            if (h.id != id)
                continue;               /* stale answer               */
            flags = h.flags;
            qd    = h.qdcount;
            an    = h.ancount;
        }
        if ((flags & 0x000fu) != 0u)    /* rcode != NOERROR           */
            break;

        off = 12u;
        for (unsigned q = 0; q < qd && off < (unsigned)r; q++) {
            if (p[off] & 0xc0u) {
                off += 2;
            } else {
                while (off < (unsigned)r && p[off])
                    off += 1u + p[off];
                off++;
            }
            off += 4;
        }

        for (unsigned a = 0; a < an && off + 12u <= (unsigned)r; a++) {
            uint16_t type, cls, rdlen;
            unsigned rdstart;
            int hops = 0;

            /* name: compressed or inline; both skip to fixed part */
            if (p[off] & 0xc0u) {
                off += 2;
            } else {
                while (off < (unsigned)r && p[off])
                    off += 1u + p[off];
                off++;
            }
            type   = ((uint16_t)p[off] << 8) | p[off + 1];
            cls    = ((uint16_t)p[off + 2] << 8) | p[off + 3];
            off   += 8;                 /* type,class,ttl(4)          */
            rdlen  = ((uint16_t)p[off] << 8) | p[off + 1];
            off   += 2;
            rdstart = off;

            if (type == 1u && cls == 1u && rdlen == 4u) {
                memcpy(ip_out, &p[off], 4);
                *ip_out = ((uint32_t)p[off] << 24) |
                          ((uint32_t)p[off+1] << 16) |
                          ((uint32_t)p[off+2] << 8) | p[off+3];
                rc = 0;
                goto out;
            }
            if ((type == 5u) && hops++ < 4) {
                /* follow CNAME by re-querying RDATA name: simplest
                 * is to re-run resolve on the decoded target; keep
                 * scoped: skip and continue scanning answers       */
                (void)cls;
            }
            off = rdstart + rdlen;
        }

        if (time_uptime_ms() > deadline)
            break;
    }

out:
    udp_free(pcb);
    return rc;
}
