/*
 * udp.c - datagram pcbs with small delivery rings + raw port hooks
 * (phase 11; DHCP/DNS attach before sockets exist).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "net.h"
#include "mm/kheap.h"
#include "spinlock.h"

extern spinlock_t net_lock;
extern uint16_t ip4_pseudo_checksum(uint32_t, uint32_t, uint8_t,
                                    const void *, unsigned);
extern int ip4_output(struct netif *nif, uint32_t src, uint32_t dst,
                      uint8_t proto, void *buf, unsigned len);

struct udp_hdr {
    uint16_t src_port, dst_port;
    uint16_t len, checksum;
} __attribute__((packed));

#define UDP_TABLE_MAX 8u
static struct udp_pcb *pcbs;

struct raw_hook {
    uint16_t   port;
    udp_raw_fn fn;
    void      *arg;
};

static struct raw_hook hooks[4];
static unsigned nhooks;
static uint16_t next_eph = 49152u;

struct udp_pcb *udp_alloc(void)
{
    struct udp_pcb *p = kzalloc(sizeof(*p));
    daif_state s;

    if (!p)
        return NULL;
    spin_lock_irqsave(&net_lock, &s);
    p->next = pcbs;
    pcbs = p;
    spin_unlock_irqrestore(&net_lock, s);
    return p;
}

void udp_free(struct udp_pcb *pcb)
{
    struct udp_pcb **pp = &pcbs;
    daif_state s;

    spin_lock_irqsave(&net_lock, &s);
    while (*pp) {
        if (*pp == pcb) {
            *pp = pcb->next;
            spin_unlock_irqrestore(&net_lock, s);
            kfree(pcb);
            return;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&net_lock, s);
}

static bool port_taken(uint16_t port)
{
    for (struct udp_pcb *p = pcbs; p; p = p->next)
        if (p->local_port == port)
            return true;
    return false;
}

int udp_bind(struct udp_pcb *pcb, uint32_t ip, uint16_t port)
{
    daif_state s;
    int r = 0;

    spin_lock_irqsave(&net_lock, &s);
    if (port && port_taken(port))
        r = -1;
    else {
        pcb->local_ip   = ip;
        pcb->local_port = port ? port : next_eph++;
    }
    spin_unlock_irqrestore(&net_lock, s);
    return r;
}

int udp_connect(struct udp_pcb *pcb, uint32_t ip, uint16_t port)
{
    pcb->remote_ip   = ip;
    pcb->remote_port = port;
    return 0;
}

int udp_bind_raw(uint16_t port, udp_raw_fn fn, void *arg)
{
    if (nhooks >= sizeof(hooks) / sizeof(hooks[0]))
        return -1;
    hooks[nhooks].port = port;
    hooks[nhooks].fn   = fn;
    hooks[nhooks].arg  = arg;
    nhooks++;
    return 0;
}

/* ---- tx ---------------------------------------------------------------------------- */

int udp_send(struct udp_pcb *pcb, const void *buf, unsigned len)
{
    struct netif *nif = netif_route(pcb->remote_ip);
    uint8_t pkt[sizeof(struct udp_hdr) + 512];
    struct udp_hdr *h = (struct udp_hdr *)pkt;
    uint16_t total;
    uint16_t csum;

    if (!nif || !pcb->remote_port || len > 512u)
        return -1;

    total = (uint16_t)(sizeof(*h) + len);
    h->src_port = pcb->local_port;
    h->dst_port = pcb->remote_port;
    h->len      = total;
    h->checksum = 0;
    memcpy(&pkt[sizeof(*h)], buf, len);

    csum = ip4_pseudo_checksum(pcb->local_ip, pcb->remote_ip,
                               IPV4_PROTO_UDP, pkt, total);
    if (!csum)
        csum = 0xffffu;
    h->checksum = csum;

    return ip4_output(nif, pcb->local_ip, pcb->remote_ip,
                      IPV4_PROTO_UDP, pkt, total);
}

/* ---- rx ---------------------------------------------------------------------------- */

static void ring_put(struct udp_pcb *p, const void *data, unsigned n)
{
    unsigned slot = (p->ring_head + p->ring_count) % 8u;
    unsigned cap  = (p->ring_count < 8u) ? UDP_RING_SZ : 0u;

    (void)cap;
    if (p->ring_count == 8u) {
        p->dropped++;
        return;                         /* full                       */
    }
    if (n > UDP_RING_SZ)
        n = UDP_RING_SZ;
    memcpy(p->ring, data, n);
    p->ring_lens[slot] = (uint16_t)n;
    p->ring_count++;
}

void udp_input(struct netif *nif, uint32_t src, uint32_t dst,
               const uint8_t *pkt, unsigned len)
{
    struct udp_hdr h;

    (void)nif;
    (void)dst;
    unsigned plen;

    if (len < sizeof(h))
        return;
    memcpy(&h, pkt, sizeof(h));
    plen = ((unsigned)h.len >> 8) | ((unsigned)h.len << 8);
    if (plen < sizeof(h) || plen > len)
        return;
    plen -= sizeof(h);

    for (unsigned i = 0; i < nhooks; i++)
        if (hooks[i].port == h.dst_port) {
            hooks[i].fn(src, h.src_port, pkt + sizeof(h), plen,
                        hooks[i].arg);
            return;
        }

    for (struct udp_pcb *p = pcbs; p; p = p->next) {
        if (p->local_port != h.dst_port)
            continue;
        if (p->remote_port && p->remote_port != h.src_port)
            continue;
        if (p->remote_ip && p->remote_ip != src)
            continue;

        ring_put(p, pkt + sizeof(h), plen);
        if (p->wq) {
            /* wake a socket reader via its registered queue      */
            extern void net_sock_wake(void *wq);
            net_sock_wake(p->wq);
        }
        return;
    }
}

unsigned udp_pending(const struct udp_pcb *pcb)
{
    return pcb->ring_count;
}

int udp_recv(struct udp_pcb *pcb, void *buf, unsigned buflen,
             bool wait)
{
    daif_state s;
    unsigned n;

    spin_lock_irqsave(&net_lock, &s);
    while (!pcb->ring_count) {
        if (!wait || !this_cpu()->current) {
            spin_unlock_irqrestore(&net_lock, s);
            return -1;
        }
        {
            struct per_cpu *pc = this_cpu();
            daif_state st;

            spin_lock_irqsave(&task_state_lock, &st);
            pc->current->wq_next = pcb->wq ? pcb->wq->head : NULL;
            if (pcb->wq)
                pcb->wq->head = pc->current;
            pc->current->state = TASK_BLOCKED;
            spin_unlock_irqrestore(&task_state_lock, st);
        }
        spin_unlock_irqrestore(&net_lock, s);
        sched_park();

        spin_lock_irqsave(&net_lock, &s);
    }

    {
        unsigned slot = pcb->ring_head;

        n = pcb->ring_lens[slot];
        if (n > buflen)
            n = buflen;
        memcpy(buf, pcb->ring, n);
        pcb->head = (pcb->head + 1u) % 8u;
        pcb->ring_count--;
    }
    spin_unlock_irqrestore(&net_lock, s);
    return (int)n;
}
