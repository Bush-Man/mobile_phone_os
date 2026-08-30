/*
 * etharp.c - ARP cache, request/reply handling and the queued-send
 * path (phase 11). One outstanding queued frame per unresolved
 * target; requests re-arm on a 1 s timer up to 3 tries.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "net.h"
#include "time.h"

struct arp_entry {
    bool     valid;
    uint32_t ip;
    uint8_t  hw[ETH_HWADDR_LEN];
    uint64_t last_seen_ms;
};

struct arp_pending {
    bool        used;
    uint32_t    ip;
    struct netif *nif;
    uint8_t     frame[ETH_HDR_LEN + ETH_MTU];
    unsigned    len;
    uint64_t    sent_ms;
    unsigned    tries;
};

static struct arp_entry cache[ARP_TABLE_MAX];
static struct arp_pending pending;
static uint8_t zero_hw[ETH_HWADDR_LEN];

/* ---- wire helpers ------------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void ip4_pack(uint8_t *out, uint32_t ip)
{
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)ip;
}

uint32_t ip4_unpack(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | in[3];
}

/* ---- cache ---------------------------------------------------------------------------- */

static void cache_put(uint32_t ip, const uint8_t *hw, uint64_t now)
{
    struct arp_entry *slot = NULL;

    for (unsigned i = 0; i < ARP_TABLE_MAX; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            slot = &cache[i];
            break;
        }
        if (!slot && !cache[i].valid)
            slot = &cache[i];
    }
    if (!slot)
        slot = &cache[0];               /* overwrite oldest-ish       */

    slot->valid = true;
    slot->ip    = ip;
    memcpy(slot->hw, hw, ETH_HWADDR_LEN);
    slot->last_seen_ms = now;
}

int arp_lookup(uint32_t ip, uint8_t *hw_out)
{
    for (unsigned i = 0; i < ARP_TABLE_MAX; i++)
        if (cache[i].valid && cache[i].ip == ip) {
            memcpy(hw_out, cache[i].hw, ETH_HWADDR_LEN);
            return 0;
        }
    return -1;
}

unsigned arp_cache_entries(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < ARP_TABLE_MAX; i++)
        n += cache[i].valid;
    return n;
}

/* ---- wire format ------------------------------------------------------------------------ */

#define ARP_HTYPE_ETH   1u
#define ARP_REQUEST     1u
#define ARP_REPLY       2u

static void arp_build(uint8_t *pkt, uint16_t op, const uint8_t *src_hw,
                      uint32_t src_ip, const uint8_t *dst_hw,
                      uint32_t dst_ip)
{
    put16(&pkt[0], ARP_HTYPE_ETH);
    put16(&pkt[2], ETHERTYPE_IPV4);
    pkt[4] = 6;                         /* hw len                     */
    pkt[5] = 4;                         /* proto len                  */
    put16(&pkt[6], op);
    memcpy(&pkt[8], src_hw, 6);
    put32(&pkt[14], src_ip);
    memcpy(&pkt[18], dst_hw, 6);
    put32(&pkt[24], dst_ip);
}

static void arp_send_raw(struct netif *nif, const uint8_t *dst_hw,
                         const uint8_t *arp_pkt)
{
    /* link_out builds the Ethernet header: hand it the payload only */
    nif->link_out(nif, dst_hw, ETHERTYPE_ARP, arp_pkt, 28);
}

static void arp_request(struct netif *nif, uint32_t target_ip)
{
    uint8_t pkt[28];
    static const uint8_t bcast[ETH_HWADDR_LEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    arp_build(pkt, ARP_REQUEST, nif->hwaddr, nif->ip_addr,
              zero_hw, target_ip);
    arp_send_raw(nif, bcast, pkt);
}

void arp_input(const uint8_t *pkt, unsigned len, struct netif *nif)
{
    uint16_t op;
    uint32_t sender_ip, target_ip;
    uint64_t now;

    if (len < 28)
        return;
    op         = get16(&pkt[6]);
    sender_ip  = get32(&pkt[14]);
    target_ip  = get32(&pkt[24]);

    if (!sender_ip)
        return;

    /* learn the sender unconditionally (gratuitous + replies)     */
    now = time_uptime_ms();
    cache_put(sender_ip, &pkt[8], now);

    /* a queued frame waiting on this address can go out now       */
    if (pending.used && pending.ip == sender_ip) {
        struct netif *out = pending.nif;
        unsigned plen = pending.len;

        pending.used = false;
        out->link_out(out, &pkt[8], ETHERTYPE_IPV4,
                      pending.frame, plen);
    }

    if (op == ARP_REQUEST && target_ip == nif->ip_addr) {
        uint8_t reply[28];

        arp_build(reply, ARP_REPLY, nif->hwaddr, nif->ip_addr,
                  &pkt[8], sender_ip);
        arp_send_raw(nif, &pkt[8], reply);
    }
}

void arp_tick(uint64_t now_ms)
{
    if (!pending.used)
        return;

    if (now_ms - pending.sent_ms >= 1000u) {
        if (++pending.tries > 3u) {
            pending.used = false;       /* give up                    */
            return;
        }
        pending.sent_ms = now_ms;
        arp_request(pending.nif, pending.ip);
    }
}

int arp_send_ip(struct netif *nif, uint32_t dst_ip,
                const void *buf, unsigned len)
{
    uint8_t hw[ETH_HWADDR_LEN];

    if (len > ETH_MTU)
        return -1;

    if (arp_lookup(dst_ip, hw) == 0)
        return nif->link_out(nif, hw, ETHERTYPE_IPV4, buf, len);

    /* unresolved: queue the payload (single slot) and request       */
    if (pending.used && pending.ip == dst_ip)
        return -1;                      /* already waiting            */

    pending.used = true;
    pending.ip   = dst_ip;
    pending.nif  = nif;
    pending.len  = len;
    pending.sent_ms = 0;
    pending.tries   = 0;

    memcpy(pending.frame, buf, len);

    arp_request(nif, dst_ip);
    return 0;
}
