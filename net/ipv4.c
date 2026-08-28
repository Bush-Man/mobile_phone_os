/*
 * ipv4.c - header build/parse, checksums and protocol demux
 * (phase 11). Fragmentation is not supported: DF set, oversized
 * datagrams dropped and counted.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "net.h"

uint16_t ip4_checksum(const void *buf, unsigned len)
{
    const uint8_t *p = buf;
    uint32_t sum = 0;

    for (unsigned i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (len & 1u)
        sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

uint16_t ip4_pseudo_checksum(uint32_t src, uint32_t dst,
                             uint8_t proto, const void *hdr_payload,
                             unsigned len)
{
    uint32_t sum = 0;

    sum += (src >> 16) & 0xffffu;
    sum += src & 0xffffu;
    sum += (dst >> 16) & 0xffffu;
    sum += dst & 0xffffu;
    sum += proto;
    sum += len;

    {
        const uint8_t *p = hdr_payload;

        for (unsigned i = 0; i + 1 < len; i += 2)
            sum += ((uint32_t)p[i] << 8) | p[i + 1];
        if (len & 1u)
            sum += (uint32_t)p[len - 1] << 8;
    }
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

/* ---- tx ------------------------------------------------------------------------ */

/* payload: L4 header + data; writes ipv4 header at buf[-20]       */
int ip4_output(struct netif *nif, uint32_t src, uint32_t dst,
               uint8_t proto, void *buf, unsigned len)
{
    uint8_t *ip = (uint8_t *)buf - 20u;
    uint16_t total = (uint16_t)(20u + len);

    if (len > ETH_MTU - 20u)
        return -1;

    ip[0] = 0x45;                       /* v4, IHL 5                  */
    ip[1] = 0;                          /* DSCP/ECN                   */
    ip[2] = (uint8_t)(total >> 8);
    ip[3] = (uint8_t)total;
    ip[4] = 0x40;                       /* DF                         */
    ip[5] = 0;
    ip[6] = ip[7] = 0;                  /* id/frag                    */
    ip[8] = 64;                         /* TTL                        */
    ip[9] = proto;
    ip[10] = ip[11] = 0;
    ip[12] = (uint8_t)(src >> 24);
    ip[13] = (uint8_t)(src >> 16);
    ip[14] = (uint8_t)(src >> 8);
    ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dst >> 24);
    ip[17] = (uint8_t)(dst >> 16);
    ip[18] = (uint8_t)(dst >> 8);
    ip[19] = (uint8_t)dst;

    {
        uint16_t csum = ip4_checksum(ip, 20);

        ip[10] = (uint8_t)(csum >> 8);
        ip[11] = (uint8_t)csum;
    }

    if (dst == IP4_BROADCAST)
        return arp_send_ip(nif, IP4_BROADCAST, ip, total);
    return arp_send_ip(nif, dst, ip, total);
}

/* ---- rx ---------------------------------------------------------------------------- */

static void ip4_dump_drop(struct netif *nif, unsigned len)
{
    (void)nif;
    (void)len;
}

void ipv4_input(struct netif *nif, const uint8_t *pkt, unsigned len)
{
    unsigned ihl;
    uint16_t total;
    uint8_t proto;
    uint32_t src, dst;
    uint16_t csum_rx, csum_calc;

    if (len < 20)
        return;
    if ((pkt[0] >> 4) != 4)
        return;

    ihl   = (pkt[0] & 0x0fu) * 4u;
    total = ((uint16_t)pkt[2] << 8) | pkt[3];

    if (ihl < 20 || ihl > len || total > len || total < ihl)
        return;

    csum_rx  = ((uint16_t)pkt[10] << 8) | pkt[11];
    csum_calc = ip4_checksum(pkt, ihl);
    if (csum_calc != 0u && csum_rx != csum_calc)
        return;

    proto = pkt[9];
    src   = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
            ((uint32_t)pkt[14] << 8) | pkt[15];
    dst   = ((uint32_t)pkt[16] << 24) | ((uint32_t)pkt[17] << 16) |
            ((uint32_t)pkt[18] << 8) | pkt[19];

    if (dst != nif->ip_addr && dst != IP4_BROADCAST &&
        dst != IP4_LOOPBACK)
        return;                         /* not for us                 */

    switch (proto) {
    case IPV4_PROTO_ICMP:
        icmp_input(nif, src, dst, pkt + ihl, total - ihl);
        break;
    case IPV4_PROTO_UDP:
        udp_input(nif, src, dst, pkt + ihl, total - ihl);
        break;
    case IPV4_PROTO_TCP:
        tcp_input(nif, src, dst, pkt + ihl, total - ihl);
        break;
    default:
        ip4_dump_drop(nif, total);
        break;
    }
}
