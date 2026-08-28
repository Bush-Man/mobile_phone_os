/*
 * dhcp.c - DISCOVER/OFFER/REQUEST/ACK over UDP (phase 11, item 61).
 *
 * The xid is derived from uptime; options parsed: 53 (msg type),
 * 1 (mask), 3 (router), 6 (DNS), 51 (lease), 54 (server id).
 * QEMU SLIRP answers deterministically; on timeout the caller falls
 * back to the well-known SLIRP statics (10.0.2.15/24 via 10.0.2.2).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "net.h"
#include "etharp.h"
#include "time.h"

#define DHCP_PORT_SERVER 67u
#define DHCP_PORT_CLIENT 68u

#define DHCPDISCOVER 1u
#define DHCPOFFER    2u
#define DHCPREQUEST  3u
#define DHCPACK      5u

#define OPT_MASK     1u
#define OPT_ROUTER   3u
#define OPT_DNS      6u
#define OPT_LEASE    51u
#define OPT_MSGTYPE  53u
#define OPT_SERVERID 54u
#define OPT_END      255u

struct dhcp_hdr {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16], sname[64], file[128];
    uint32_t magic;
} __attribute__((packed));

#define DHCP_MAGIC 0x63825363u

static uint8_t  dc_last_ip_tmp[4];
static uint8_t  dc_ack_yiaddr[4];
static bool     dc_offer_seen, dc_ack_seen;
static uint32_t dc_server_id;

static struct {
    uint32_t xid;
    struct dhcp_result last;
    bool have;
} dc;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void dhcp_raw_sink(uint32_t src_ip, uint16_t src_port,
                          const void *payload, unsigned len, void *arg)
{
    const struct dhcp_hdr *h = payload;
    const uint8_t *opt;
    unsigned olen, i = 240u;
    uint8_t msgtype = 0;
    uint32_t server_id = 0;

    (void)src_ip; (void)src_port; (void)arg;
    if (len < 240u || get32((const uint8_t *)&h->magic) != DHCP_MAGIC)
        return;
    if (dc.xid && h->xid != dc.xid)
        return;

    opt = payload;
    /* scan options tail */
    while (i + 1 < len) {
        uint8_t code = opt[i];

        if (code == OPT_END)
            break;
        if (code == 0) { i++; continue; }
        olen = opt[i + 1];
        if (i + 2 + olen > len)
            break;

        switch (code) {
        case OPT_MSGTYPE:
            msgtype = opt[i + 2];
            break;
        case OPT_SERVERID:
            if (olen >= 4) server_id = get32(&opt[i + 2]);
            break;
        case OPT_MASK:
            if (olen >= 4 && dc.have)
                dc.last.netmask = get32(&opt[i + 2]);
            break;
        case OPT_ROUTER:
            if (olen >= 4 && dc.have)
                dc.last.gw = get32(&opt[i + 2]);
            break;
        case OPT_DNS:
            if (olen >= 4 && dc.have)
                dc.last.dns = get32(&opt[i + 2]);
            break;
        case OPT_LEASE:
            if (olen >= 4 && dc.have)
                dc.last.lease_sec = get32(&opt[i + 2]);
            break;
        default:
            break;
        }
        i += 2u + olen;
    }

        if (msgtype == DHCPOFFER && !dc.have) {
        memcpy(dc_last_ip_tmp, h->yiaddr, 4);
        dc_offer_seen = true;
        dc_server_id  = server_id;
    } else if (msgtype == DHCPACK) {
        memcpy(dc_ack_yiaddr, h->yiaddr, 4);
        dc_ack_seen = true;
    }
}

static void dhcp_fill_tail(uint8_t *pkt, uint8_t msgtype,
                           const uint8_t *server_id_opt)
{
    unsigned o = 240u;

    pkt[o++] = OPT_MSGTYPE; pkt[o++] = 1; pkt[o++] = msgtype;
    if (msgtype == DHCPREQUEST && server_id_opt) {
        pkt[o++] = OPT_SERVERID; pkt[o++] = 4;
        memcpy(&pkt[o], server_id_opt, 4); o += 4;
    }
    pkt[o++] = OPT_ROUTER; pkt[o++] = 4;
    put32(&pkt[o], 0); o += 4;
    pkt[o++] = OPT_END;
    while (o < 300u) pkt[o++] = 0;
}

static void dhcp_build(uint8_t *pkt, uint8_t msgtype,
                       struct netif *nif, const uint8_t *server_id_opt)
{
    struct dhcp_hdr *h = (struct dhcp_hdr *)pkt;

    memset(pkt, 0, 300u);
    h->op = 1; h->htype = 1; h->hlen = 6;
    h->xid = dc.xid;
    h->secs = 0;
    h->flags = 0x8000u >> 8;            /* broadcast bit, BE         */
    memcpy(h->chaddr, nif->hwaddr, 6);
    {
        /* magic 0x63825363 on the wire = bytes 63 82 53 63         */
        uint8_t *m = (uint8_t *)&h->magic;

        m[0] = 0x63; m[1] = 0x82; m[2] = 0x53; m[3] = 0x63;
    }
    dhcp_fill_tail(pkt, msgtype, server_id_opt);
}

int dhcp_discover(struct netif *nif, struct dhcp_result *out,
                  uint32_t timeout_ms)
{
    struct udp_pcb *pcb = udp_alloc();
    uint8_t pkt[300];
    uint64_t deadline;
    int rc = -1;

    if (!pcb)
        return -1;

    memset(&dc.last, 0, sizeof(dc.last));
    dc.have = false;
    dc_offer_seen = dc_ack_seen = false;
    dc.xid = (uint32_t)time_uptime_ms() | 1u;

    udp_bind(pcb, IP4_ANY, DHCP_PORT_CLIENT);
    udp_connect(pcb, IP4_BROADCAST, DHCP_PORT_SERVER);
    udp_bind_raw(DHCP_PORT_CLIENT, dhcp_raw_sink, NULL);

    dhcp_build(pkt, DHCPDISCOVER, nif, NULL);
    udp_send(pcb, pkt, 300u);

    deadline = time_uptime_ms() + timeout_ms / 2u;
    while (!dc_offer_seen && time_uptime_ms() < deadline)
        msleep(4);

    if (!dc_offer_seen)
        goto out;

    dc.have = true;                     /* options land from now on  */
    memcpy(&dc.last.ip, dc_last_ip_tmp, 4);
    {
        uint8_t sid[4];

        put32(sid, dc_server_id);
        dhcp_build(pkt, DHCPREQUEST, nif, sid);
    }
    udp_send(pcb, pkt, 300u);

    deadline = time_uptime_ms() + timeout_ms / 2u;
    while (!dc_ack_seen && time_uptime_ms() < deadline)
        msleep(4);

    if (!dc_ack_seen)
        goto out;

    dc.last.bound = true;
    dc.last.ip    = ip4_unpack(dc_last_ip_tmp);
    if (!dc.last.gw)     dc.last.gw  = IP4_SLIRP_GW;
    if (!dc.last.dns)    dc.last.dns = IP4_SLIRP_DNS;
    if (!dc.last.netmask) dc.last.netmask = 0xffffff00u;

    nif->ip_addr = dc.last.ip;
    nif->netmask = dc.last.netmask;
    nif->gw      = dc.last.gw;
    memcpy(out, &dc.last, sizeof(*out));
    rc = 0;

out:
    udp_free(pcb);
    return rc;
}
