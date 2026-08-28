/*
 * icmp.c - echo reply (ping answers) + a blocking ping requestor
 * with id/seq matching (phase 11).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "net.h"
#include "spinlock.h"
#include "task.h"
#include "time.h"

struct icmp_echo_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct ping_slot {
    volatile bool done;
    bool     ok;
    uint16_t id, seq;
};

static struct ping_slot waiter;

/* ---- rx ---------------------------------------------------------------------------- */

void icmp_input(struct netif *nif, uint32_t src, uint32_t dst,
                const void *hdr, unsigned len)
{
    const struct icmp_echo_hdr *e = hdr;

    if (len < sizeof(*e))
        return;

    if (e->type == ICMP_ECHO_REQUEST) {
        /* echo it back: copy, flip type, fix checksum            */
        uint8_t resp[64 + 56];
        struct icmp_echo_hdr *r = (struct icmp_echo_hdr *)resp;
        unsigned plen = len;

        if (plen > sizeof(resp))
            plen = sizeof(resp);
        memcpy(resp, hdr, plen);
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = ip4_checksum(resp, plen);
        {
            uint8_t framed[64 + 56 + 20];

            framed[0] = 0x45;
            framed[1] = 0;
            framed[2] = (uint8_t)((20u + plen) >> 8);
            framed[3] = (uint8_t)(20u + plen);
            framed[8] = 64;
            framed[9] = IPV4_PROTO_ICMP;
            framed[10] = framed[11] = 0;
            framed[12] = (uint8_t)(dst >> 24);
            framed[13] = (uint8_t)(dst >> 16);
            framed[14] = (uint8_t)(dst >> 8);
            framed[15] = (uint8_t)dst;
            framed[16] = (uint8_t)(src >> 24);
            framed[17] = (uint8_t)(src >> 16);
            framed[18] = (uint8_t)(src >> 8);
            framed[19] = (uint8_t)src;
            {
                uint16_t c = ip4_checksum(framed, 20);

                framed[10] = (uint8_t)(c >> 8);
                framed[11] = (uint8_t)c;
            }
            memcpy(&framed[20], resp, plen);
            arp_send_ip(nif, src, framed, 20u + plen);
            (void)0;
        }
        return;
    }

    if (e->type == ICMP_ECHO_REPLY && waiter.id == e->id &&
        waiter.seq == e->seq && !waiter.done) {
        waiter.ok   = true;
        waiter.done = true;
    }
}

int icmp_ping(uint32_t dst, uint16_t id, uint16_t seq,
              uint32_t timeout_ms)
{
    struct netif *nif = netif_route(dst);
    uint8_t pkt[64];
    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)pkt;
    uint64_t start, deadline;
    int rc = -1;

    if (!nif)
        return -1;

    memset(pkt, 0, sizeof(pkt));
    e->type = ICMP_ECHO_REQUEST;
    e->code = 0;
    e->id   = id;
    e->seq  = seq;
    for (unsigned i = 0; i < 32u; i++)
        pkt[8 + i] = (uint8_t)('A' + i % 26u);
    e->checksum = ip4_checksum(pkt, 8u + 32u);

    waiter.id  = id;
    waiter.seq = seq;
    waiter.ok  = false;
    waiter.done = false;

    start = time_uptime_ms();
    {
        uint8_t framed[20 + sizeof(pkt)];

        framed[0] = 0x45;
        framed[1] = 0;
        framed[2] = 0;
        framed[3] = (uint8_t)(20u + sizeof(pkt));
        framed[8] = 64;
        framed[9] = IPV4_PROTO_ICMP;
        framed[10] = framed[11] = 0;
        {
            uint32_t src = nif->ip_addr;

            framed[12] = (uint8_t)(src >> 24);
            framed[13] = (uint8_t)(src >> 16);
            framed[14] = (uint8_t)(src >> 8);
            framed[15] = (uint8_t)src;
        }
        framed[16] = (uint8_t)(dst >> 24);
        framed[17] = (uint8_t)(dst >> 16);
        framed[18] = (uint8_t)(dst >> 8);
        framed[19] = (uint8_t)dst;
        {
            uint16_t c = ip4_checksum(framed, 20);

            framed[10] = (uint8_t)(c >> 8);
            framed[11] = (uint8_t)c;
        }
        memcpy(&framed[20], pkt, sizeof(pkt));
        if (arp_send_ip(nif, dst, framed, 20u + sizeof(pkt)))
            return -1;
    }

    deadline = time_uptime_ms() + timeout_ms;
    while (!waiter.done) {
        if ((long)(time_uptime_ms() - deadline) >= 0)
            goto out;
        msleep(2);
    }
    rc = waiter.ok ? (int)(time_uptime_ms() - start) : -1;
out:
    return rc;
}
