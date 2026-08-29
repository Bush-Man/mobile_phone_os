/*
 * netif.c - interface registry, routing and the loopback netif
 * (phase 11, plan item 58).
 *
 * net_lock guards every table in the stack (netifs, arp, udp/tcp
 * pcbs, checksum scratch). RX enters through netif_input() from the
 * virtio bottom half; TX paths may sleep because virtio_net_send
 * polls its completion.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "lib.h"
#include "net.h"
#include "spinlock.h"

void arp_input(const uint8_t *pkt, unsigned len, struct netif *nif);
void ipv4_input(struct netif *nif, const uint8_t *pkt, unsigned len);
void tcp_timers_tick(uint64_t now_ms);
void dhcp_tick(uint64_t now_ms);
void udp_input(struct netif *nif, uint32_t src, uint32_t dst,
              const uint8_t *pkt, unsigned len);
void tcp_input(struct netif *nif, uint32_t src, uint32_t dst,
               const uint8_t *pkt, unsigned len);


struct netif *net_default_g;
static struct netif *netifs;
static unsigned nnetifs;
spinlock_t net_lock = SPINLOCK_INIT;

/* ---- registry ---------------------------------------------------------------------- */

int netif_register(struct netif *nif)
{
    daif_state s;

    if (!nif || !nif->name)
        return -1;

    spin_lock_irqsave(&net_lock, &s);
    nif->next = netifs;
    netifs = nif;
    nnetifs++;
    spin_unlock_irqrestore(&net_lock, s);
    return 0;
}

void netif_set_default(struct netif *nif)
{
    net_default_g = nif;
}

struct netif *netif_default(void)
{
    return net_default_g;
}

struct netif *netif_find_name(const char *name)
{
    for (struct netif *it = netifs; it; it = it->next)
        if (!strcmp(it->name, name))
            return it;
    return NULL;
}

struct netif *netif_loopback(void)
{
    return netif_find_name("lo");
}

unsigned netif_count(void)
{
    return nnetifs;
}

/*
 * Phase 14: registry-order accessor for the SYS_netinfo report --
 * idx runs newest-first, matching netif_register's list push, so
 * report output is stable and lo (registered first) comes last.
 */
struct netif *netif_at(unsigned idx)
{
    struct netif *it = netifs;

    while (it && idx--)
        it = it->next;
    return it;
}

struct netif *netif_route(uint32_t dst_ip)
{
    struct netif *lo = NULL;

    if (dst_ip == IP4_LOOPBACK)
        return netif_loopback();

    for (struct netif *it = netifs; it; it = it->next) {
        if (it->is_loopback) {
            lo = it;
            continue;
        }
        if ((dst_ip & it->netmask) == (it->ip_addr & it->netmask))
            return it;                 /* directly connected         */
    }
    return net_default_g ? net_default_g : lo;
}

/* ---- loopback netif ------------------------------------------------------------------ */

static int lo_out(struct netif *nif, const uint8_t *dest_hw,
                  uint16_t ethertype, const void *buf, unsigned len)
{
    uint8_t frame[ETH_HDR_LEN + ETH_MTU];

    (void)dest_hw;
    if (len > ETH_MTU)
        return -1;

    memset(frame, 0, ETH_HDR_LEN);
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)ethertype;
    memcpy(&frame[ETH_HDR_LEN], buf, len);

    /* re-inject synchronously: full stack round trip, no DMA      */
    netif_input(frame, len + ETH_HDR_LEN, nif);
    return 0;
}

static struct netif lo_netif = {
    .name        = "lo",
    .ip_addr     = IP4_LOOPBACK,
    .netmask     = 0xff000000u,
    .mtu         = ETH_MTU,
    .is_loopback = true,
    .up          = true,
    .link_out    = lo_out,
};

int lo_netif_register(void)
{
    return netif_register(&lo_netif);
}

/* ---- input dispatch -------------------------------------------------------------------- */

static void eth_input(const uint8_t *frame, unsigned len,
                      struct netif *nif)
{
    uint16_t ethertype;

    if (len < ETH_HDR_LEN)
        return;
    ethertype = ((uint16_t)frame[12] << 8) | frame[13];

    switch (ethertype) {
    case ETHERTYPE_ARP:
        arp_input(frame + ETH_HDR_LEN, len - ETH_HDR_LEN, nif);
        break;
    case ETHERTYPE_IPV4:
        ipv4_input(nif, frame + ETH_HDR_LEN,
                   len - ETH_HDR_LEN);
        break;
    default:
        break;
    }
}

void netif_input(const void *frame, unsigned len, void *arg)
{
    eth_input((const uint8_t *)frame, len, (struct netif *)arg);
}

/* ---- stack timers ------------------------------------------------------------------------- */

void net_timers_tick(uint64_t now_ms)
{
    arp_tick(now_ms);
    tcp_timers_tick(now_ms);
    dhcp_tick(now_ms);
}

/* ---- internal APIs shared across net/ (defined in later files)  ---- */

