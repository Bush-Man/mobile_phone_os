/*
 * tcp.c - compact TCP: pcb table, client+server handshakes, single-
 * outstanding-segment send with backoff retransmission, in-order
 * receive rings and graceful close (phase 11, item 59).
 *
 * Deliberate simplifications (all documented): no out-of-order
 * queueing (segments with seq != rcv_nxt are dropped and the last
 * ACK re-sent), no window probing, TIME_WAIT collapses immediately,
 * and the advertised window is fixed TCP_WINDOW.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "mm/kheap.h"
#include "net.h"
#include "spinlock.h"
#include "time.h"

extern int ip4_output(struct netif *nif, uint32_t src, uint32_t dst,
                      uint8_t proto, void *buf, unsigned len);

static struct waitqueue *listener_wq_of(struct tcp_pcb *p)
{
    return p->wq;
}

struct tcp_hdr {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

#define SEQ_LT(a,b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a,b) ((int32_t)((a) - (b)) <= 0)

#define TCP_TABLE_MAX 16u
#define TCP_BACKOFF_BASE_MS 200u
#define TCP_ISN() (time_uptime_ms() * 1000u + 1u)

static struct tcp_pcb *pcbs;
static uint16_t eph = 49152u;
extern spinlock_t net_lock;
extern void net_sock_wake(void *wq);

/* ---- alloc/table -------------------------------------------------------------------- */

struct tcp_pcb *tcp_alloc(void)
{
    struct tcp_pcb *p = kzalloc(sizeof(*p));
    daif_state s;

    if (!p)
        return NULL;
    spin_lock_irqsave(&net_lock, &s);
    p->next = pcbs;
    pcbs = p;
    spin_unlock_irqrestore(&net_lock, s);
    return p;
}

void tcp_free(struct tcp_pcb *pcb)
{
    struct tcp_pcb **pp = &pcbs;
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

struct tcp_pcb *tcp_next_pcb(struct tcp_pcb *cur)
{
    return cur ? cur->next : pcbs;
}

static bool port_taken(uint16_t port)
{
    for (struct tcp_pcb *p = pcbs; p; p = p->next)
        if (p->local_port == port && p->state != TCP_CLOSED)
            return true;
    return false;
}

/* ---- wire helpers ---------------------------------------------------------------------- */

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

static uint16_t build_seg(uint8_t *pkt, uint32_t src, uint32_t dst,
                          uint16_t sp, uint16_t dp, uint32_t seq,
                          uint32_t ack, uint8_t flags,
                          const void *payload, uint16_t plen)
{
    struct tcp_hdr *h = (struct tcp_hdr *)pkt;

    memset(pkt, 0, sizeof(*h));
    h->src_port = sp;
    h->dst_port = dp;
    put32(pkt + 4, seq);
    put32(pkt + 8, ack);
    h->data_off = 0x50u;
    h->flags    = flags;
    h->window   = TCP_WINDOW;
    if (plen) {
        h->checksum = 0;
        memcpy(&pkt[sizeof(*h)], payload, plen);
    }
    h->checksum = ip4_pseudo_checksum(src, dst, IPV4_PROTO_TCP,
                                      pkt, sizeof(*h) + plen);
    return (uint16_t)(sizeof(*h) + plen);
}

/* ---- low-level transmit ----------------------------------------------------------------- */

static int tcp_xmit(struct tcp_pcb *p, uint8_t flags,
                    const void *payload, uint16_t plen,
                    uint32_t seq)
{
    struct netif *nif = netif_route(p->remote_ip);
    uint8_t pkt[20 + TCP_MSS];
    uint16_t len;

    if (!nif || !p->remote_ip)
        return -1;
    len = build_seg(pkt, p->local_ip, p->remote_ip,
                    p->local_port, p->remote_port,
                    seq, p->rcv_nxt, flags, payload, plen);
    return ip4_output(nif, p->local_ip, p->remote_ip,
                      IPV4_PROTO_TCP, pkt, len);
}

/* sends a pure ACK for what we have received so far               */
static void tcp_send_ack(struct tcp_pcb *p)
{
    tcp_xmit(p, TCP_ACK, NULL, 0, p->snd_nxt);
}

/* ---- retransmit ---------------------------------------------------------------------------- */

void tcp_timers_tick(uint64_t now_ms)
{
    daif_state s;

    spin_lock_irqsave(&net_lock, &s);
    for (struct tcp_pcb *p = pcbs; p; p = p->next) {
        if (!p->snd_inflight || p->state == TCP_CLOSED)
            continue;
        if ((int64_t)(now_ms - p->snd_timeout_ms) < 0)
            continue;

        if (++p->retries > TCP_MAX_RETRIES) {
            p->rst_received = true;
            p->snd_inflight = false;
            spin_unlock_irqrestore(&net_lock, s);
            if (p->wq)
                net_sock_wake(p->wq);
            spin_lock_irqsave(&net_lock, &s);
            continue;
        }

        p->snd_timeout_ms =
            now_ms + TCP_BACKOFF_BASE_MS * (1u << (p->retries - 1u));
        tcp_xmit(p, p->fin_sent ? TCP_FIN | TCP_ACK : TCP_ACK,
                 p->snd_seg, p->snd_len, p->snd_una);
    }
    spin_unlock_irqrestore(&net_lock, s);
}

/* ---- app API (client/server) ------------------------------------------------------------ */

int tcp_bind(struct tcp_pcb *pcb, uint32_t ip, uint16_t port)
{
    daif_state s;
    int r = 0;

    spin_lock_irqsave(&net_lock, &s);
    if (port && port_taken(port))
        r = -1;
    else {
        pcb->local_ip   = ip;
        pcb->local_port = port ? port : eph++;
    }
    spin_unlock_irqrestore(&net_lock, s);
    return r;
}

int tcp_listen(struct tcp_pcb *pcb)
{
    if (pcb->state != TCP_CLOSED || !pcb->local_port)
        return -1;
    pcb->state = TCP_LISTEN;
    return 0;
}

int tcp_connect(struct tcp_pcb *pcb, uint32_t ip, uint16_t port,
                uint32_t timeout_ms)
{
    uint64_t deadline;

    if (pcb->state != TCP_CLOSED)
        return -1;

    pcb->remote_ip   = ip;
    pcb->remote_port = port;
    {
        struct netif *nif = netif_route(ip);

        pcb->local_ip = nif ? nif->ip_addr : IP4_ANY;
    }
    while (port_taken(pcb->local_port))
        pcb->local_port = eph++;

    pcb->rcv_nxt = 0;
    pcb->snd_una = pcb->snd_nxt = TCP_ISN();
    pcb->state   = TCP_SYN_SENT;

    if (tcp_xmit(pcb, TCP_SYN, NULL, 0, pcb->snd_nxt++)) {
        pcb->state = TCP_CLOSED;
        return -1;
    }
    pcb->snd_inflight    = true;
    pcb->snd_len         = 0;
    pcb->snd_timeout_ms  = time_uptime_ms() + TCP_BACKOFF_BASE_MS;

    deadline = time_uptime_ms() + timeout_ms;
    while (pcb->state == TCP_SYN_SENT) {
        if (pcb->rst_received ||
            (long)(time_uptime_ms() - deadline) >= 0) {
            pcb->state = TCP_CLOSED;
            return -1;
        }
        msleep(2);
    }
    return pcb->state == TCP_ESTABLISHED ? 0 : -1;
}

struct tcp_pcb *tcp_accept(struct tcp_pcb *listener,
                           uint32_t timeout_ms)
{
    uint64_t deadline = time_uptime_ms() + timeout_ms;

    for (;;) {
        if (listener->naccepted) {
            struct tcp_pcb *child = listener->accepted[0];

            for (unsigned i = 1; i < listener->naccepted; i++)
                listener->accepted[i - 1] = listener->accepted[i];
            listener->naccepted--;
            return child;
        }
        if (listener->state != TCP_LISTEN ||
            (timeout_ms && (long)(time_uptime_ms() - deadline) >= 0))
            return NULL;
        msleep(4);
    }
}

/* ---- app IO --------------------------------------------------------------------------- */

unsigned tcp_rcv_pending(const struct tcp_pcb *pcb)
{
    return pcb->rcv_count;
}

bool tcp_established(const struct tcp_pcb *pcb)
{
    switch (pcb->state) {
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
        return true;
    default:
        return false;
    }
}

bool tcp_eof(const struct tcp_pcb *pcb)
{
    return pcb->rcv_eof && pcb->rcv_count == 0;
}

int tcp_read(struct tcp_pcb *pcb, void *buf, unsigned len, bool wait)
{
    for (;;) {
        if (pcb->rcv_count) {
            unsigned n = pcb->rcv_count;

            if (n > len)
                n = len;
            memcpy(buf, &pcb->rcv_ring[pcb->rcv_head], n);
            pcb->rcv_head = (pcb->rcv_head + n) % TCP_RING_SZ;
            pcb->rcv_count -= n;
            return (int)n;
        }
        if (pcb->rcv_eof || pcb->rst_received)
            return 0;
        if (!wait || pcb->state == TCP_CLOSED)
            return -1;
        msleep(2);
    }
}

int tcp_write(struct tcp_pcb *pcb, const void *buf, unsigned len)
{
    const uint8_t *src = buf;
    unsigned left = len;
    int sent = 0;

    if (!tcp_established(pcb) || pcb->rst_received)
        return -1;

    while (left) {
        uint16_t n;

        while (pcb->snd_inflight) {
            if (pcb->rst_received || pcb->state == TCP_CLOSED)
                return sent ? sent : -1;
            if ((int64_t)(time_uptime_ms() -
                          (pcb->snd_timeout_ms + 5000u)) >= 0)
                return sent ? sent : -1;
            msleep(2);
        }

        n = (uint16_t)(left < TCP_MSS ? left : TCP_MSS);
        memcpy(pcb->snd_seg, src, n);
        pcb->snd_len = n;
        pcb->snd_inflight = true;
        pcb->retries = 0;
        pcb->snd_timeout_ms = time_uptime_ms() + TCP_BACKOFF_BASE_MS;

        if (tcp_xmit(pcb, TCP_ACK | TCP_PSH,
                     pcb->snd_seg, n, pcb->snd_nxt)) {
            pcb->snd_inflight = false;
            return sent ? sent : -1;
        }
        pcb->snd_nxt += n;
        src += n;
        left -= n;
        sent += (int)n;
    }
    return sent;
}

int tcp_output_pcb(struct tcp_pcb *pcb)
{
    if (pcb->state == TCP_CLOSED)
        return -1;
    tcp_send_ack(pcb);
    return 0;
}

int tcp_close(struct tcp_pcb *pcb, uint32_t timeout_ms)
{
    uint64_t deadline = time_uptime_ms() + timeout_ms;

    if (pcb->state == TCP_CLOSED)
        return 0;

    while (pcb->snd_inflight) {
        if ((long)(time_uptime_ms() - deadline) >= 0 ||
            pcb->rst_received)
            break;
        msleep(2);
    }

    pcb->fin_queued = true;
    if (pcb->state == TCP_ESTABLISHED)
        pcb->state = TCP_FIN_WAIT_1;
    else if (pcb->state == TCP_CLOSE_WAIT)
        pcb->state = TCP_LAST_ACK;
    else if (pcb->state == TCP_LISTEN || pcb->state == TCP_SYN_SENT) {
        pcb->state = TCP_CLOSED;
        tcp_free(pcb);
        return 0;
    }

    if (!pcb->snd_inflight) {
        pcb->fin_sent = true;
        pcb->snd_inflight = true;
        pcb->snd_len = 0;
        pcb->retries = 0;
        pcb->snd_timeout_ms = time_uptime_ms() + TCP_BACKOFF_BASE_MS;
        tcp_xmit(pcb, TCP_FIN | TCP_ACK, NULL, 0, pcb->snd_nxt);
        pcb->snd_nxt += 1u;
    }

    /* wait for our FIN to be acked (and, passive side, peer FIN)  */
    while (!pcb->fin_acked && pcb->state != TCP_CLOSED) {
        if (pcb->rst_received ||
            (long)(time_uptime_ms() - deadline) >= 0)
            break;
        msleep(2);
    }

    pcb->state = TCP_CLOSED;
    tcp_free(pcb);
    return 0;
}

/* ---- input path ------------------------------------------------------------------- */

static void tcp_wake(struct tcp_pcb *p)
{
    if (p->wq)
        net_sock_wake(p->wq);
}

/* child of a listener: SYN_RCVD, wired to the same ports           */
static struct tcp_pcb *tcp_spawn_child(struct tcp_pcb *listener,
                                       uint32_t src, uint16_t sport,
                                       uint32_t seq)
{
    struct tcp_pcb *c = tcp_alloc();

    if (!c)
        return NULL;
    c->state       = TCP_SYN_RCVD;
    c->local_ip    = listener->local_ip;
    c->local_port  = listener->local_port;
    c->remote_ip   = src;
    c->remote_port = sport;
    c->rcv_nxt     = seq + 1u;
    c->snd_una     = c->snd_nxt = TCP_ISN();
    c->wq          = listener->wq;
    c->sock        = NULL;

    if (listener->naccepted < 4u)
        listener->accepted[listener->naccepted++] = c;
    else {
        tcp_free(c);
        return NULL;
    }

    {
        struct netif *nif = netif_route(src);

        if (nif)
            tcp_xmit(c, TCP_SYN | TCP_ACK, NULL, 0, c->snd_nxt++);
    }
    return c;
}

void tcp_input(struct netif *nif, uint32_t src, uint32_t dst,
               const uint8_t *pkt, unsigned len)
{
    struct tcp_hdr h;

    (void)nif;
    unsigned doff, plen;
    struct tcp_pcb *p;
    uint32_t seq, ack;
    uint8_t flags;

    if (len < sizeof(h))
        return;
    memcpy(&h, pkt, sizeof(h));
    doff = (h.data_off >> 4) * 4u;
    if (doff < sizeof(h) || doff > len)
        return;
    plen  = len - doff;
    seq   = get32(pkt + 4);
    ack   = get32(pkt + 8);
    flags = h.flags;

    /* find pcb: connected match first, then listeners              */
    p = NULL;
    for (struct tcp_pcb *it = pcbs; it; it = it->next) {
        if (it->local_port != h.dst_port)
            continue;
        if (it->remote_port == h.src_port && it->remote_ip == src &&
            it->local_ip == dst)
            {
                p = it;
                break;
            }
    }
    if (!p)
        for (struct tcp_pcb *it = pcbs; it; it = it->next)
            if (it->state == TCP_LISTEN &&
                it->local_port == h.dst_port &&
                (it->local_ip == dst || it->local_ip == IP4_ANY)) {
                p = it;
                break;
            }
    if (!p)
        return;                         /* no socket: RST skipped     */

    /* ---- listener: handle SYN ----------------------------------- */
    if (p->state == TCP_LISTEN) {
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            struct tcp_pcb *c = tcp_spawn_child(p, src, h.src_port,
                                                seq);

            if (c)
                tcp_wake(p);
        }
        return;
    }

    if (p->state == TCP_CLOSED)
        return;

    if (flags & TCP_RST) {
        p->rst_received = true;
        p->snd_inflight = false;
        tcp_wake(p);
        return;
    }

    /* ---- handshake progress -------------------------------------- */
    if (p->state == TCP_SYN_SENT && (flags & TCP_SYN) &&
        (flags & TCP_ACK)) {
        p->rcv_nxt   = seq + 1u;
        p->snd_una   = ack;
        p->snd_inflight = false;
        p->state     = TCP_ESTABLISHED;
        tcp_send_ack(p);
        tcp_wake(p);
        return;
    }
    if (p->state == TCP_SYN_RCVD && (flags & TCP_ACK) &&
        !(flags & TCP_SYN)) {
        if (SEQ_LEQ(p->snd_una, ack)) {
            p->snd_una = ack;
            p->snd_inflight = false;
        }
        p->state = TCP_ESTABLISHED;
        tcp_wake(p);
        return;
    }

    /* ---- generic ACK processing ------------------------------------ */
    if (flags & TCP_ACK) {
        if (p->snd_inflight && SEQ_LEQ(p->snd_una, ack)) {
            uint32_t acked = ack - p->snd_una;

            if (acked >= p->snd_len && !p->fin_sent) {
                p->snd_inflight = false;
                p->retries = 0;
            } else if (p->fin_sent && ack == p->snd_nxt) {
                p->fin_acked    = true;
                p->snd_inflight = false;
            }
            p->snd_una = ack;
        }
    }

    /* ---- payload ------------------------------------------------------ */
    if (plen && seq == p->rcv_nxt &&
        (p->state == TCP_ESTABLISHED || p->state == TCP_FIN_WAIT_1 ||
         p->state == TCP_FIN_WAIT_2 || p->state == TCP_CLOSE_WAIT)) {
        uint16_t space = TCP_RING_SZ - p->rcv_count;
        uint16_t take = plen < space ? plen : space;

        if (take) {
            unsigned tail = (p->rcv_head + p->rcv_count) % TCP_RING_SZ;
            unsigned first = TCP_RING_SZ - tail;

            if (first > take)
                first = take;
            memcpy(&p->rcv_ring[tail], pkt + doff, first);
            memcpy(p->rcv_ring, pkt + doff + first, take - first);
            p->rcv_count += take;
            p->rcv_nxt   += take;
        }
        p->last_ack_sent = p->rcv_nxt;
    } else if (plen) {
        /* out of order / no space: re-ack current position        */
        tcp_send_ack(p);
        return;
    }

    /* ---- peer FIN -------------------------------------------------------- */
    if ((flags & TCP_FIN) && !p->peer_fin) {
        p->peer_fin = true;
        p->rcv_eof  = true;
        p->rcv_nxt  = seq + plen + 1u;

        if (p->state == TCP_ESTABLISHED)
            p->state = TCP_CLOSE_WAIT;
        else if (p->state == TCP_FIN_WAIT_1)
            p->state = TCP_CLOSING;
        else if (p->state == TCP_FIN_WAIT_2) {
            p->state = TCP_TIME_WAIT;
        }
        tcp_wake(p);
    }

    /* ---- state endings ----------------------------------------------- */
    if (p->state == TCP_FIN_WAIT_1 && p->fin_acked && p->peer_fin)
        p->state = TCP_TIME_WAIT;
    else if (p->state == TCP_CLOSING && p->fin_acked)
        p->state = TCP_TIME_WAIT;
    else if (p->state == TCP_LAST_ACK && p->fin_acked)
        p->state = TCP_CLOSED;
    else if (p->state == TCP_TIME_WAIT) {
        /* collapse: ack once more and close                       */
        tcp_send_ack(p);
        p->state = TCP_CLOSED;
    }

    if (plen || (flags & TCP_FIN) || (flags & TCP_SYN))
        tcp_send_ack(p);
    else if (p->snd_inflight && ack == p->snd_nxt)
        p->snd_inflight = false;

    tcp_wake(p);
}
