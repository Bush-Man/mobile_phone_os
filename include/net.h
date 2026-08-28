#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

#include "task.h"

/*
 * Compact self-written TCP/IP stack (phase 11, plan items 58-61).
 *
 * Layering (all files under net/):
 *
 *   sockets (AF_INET vnodes + syscalls)      net/sockets.c
 *     -> TCP pcbs / UDP pcbs                 net/tcp.c net/udp.c
 *       -> IPv4 demux + tx                   net/ipv4.c net/icmp.c
 *         -> ARP + Ethernet                  net/etharp.c
 *           -> netif registry + loopback     net/netif.c
 *             -> virtio-net (phase 6)        drivers/virtio_net.c
 *
 * Concurrency: ONE core spinlock (net_lock, in netif.c) guards all
 * tables; blocking readers park with the established subsystem ->
 * task_state two-lock order (same as input.c/sync.c). RX runs from
 * the virtio tasklet bottom half (housekeeping context), so handlers
 * copy what they keep but never sleep inside parser code; TX can
 * sleep (virtio_net_send polls completion) and therefore runs only
 * from task contexts.
 *
 * Addresses are HOST-order u32 internally; wire writes shift bytes
 * explicitly. Determinism: a loopback netif re-injects frames into
 * the input path, so ping/TCP tests run without external peers.
 */

#define ETH_HWADDR_LEN  6u
#define ETH_HDR_LEN     14u
#define ETH_MTU         1500u

#define ETHERTYPE_IPV4  0x0800u
#define ETHERTYPE_ARP   0x0806u

#define IPV4_PROTO_ICMP 1u
#define IPV4_PROTO_UDP  17u
#define IPV4_PROTO_TCP  6u

#define IP4(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                      ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define IP4_LOOPBACK    IP4(127,0,0,1)
#define IP4_BROADCAST   0xffffffffu
#define IP4_ANY         0u

/* well-known SLIRP (QEMU user-net) addresses, used as fallbacks   */
#define IP4_SLIRP_GW    IP4(10,0,2,2)
#define IP4_SLIRP_GUEST IP4(10,0,2,15)
#define IP4_SLIRP_DNS   IP4(10,0,2,3)

static inline uint32_t ip4_mask_to_prefix(uint32_t mask)
{
    unsigned n = 0;

    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

/* ---- netif ------------------------------------------------------------------------ */

struct netif {
    const char  *name;
    uint8_t      hwaddr[ETH_HWADDR_LEN];

    uint32_t     ip_addr, netmask, gw;

    unsigned     mtu;
    bool         is_loopback;
    bool         up;

    /* returns 0 / -1; may sleep (task-context callers only)        */
    int (*link_out)(struct netif *nif, const uint8_t *dest_hw,
                    uint16_t ethertype, const void *buf, unsigned len);

    void        *priv;
    struct netif *next;
};

int  netif_register(struct netif *nif);
void netif_set_default(struct netif *nif);
struct netif *netif_default(void);
struct netif *netif_find_name(const char *name);
struct netif *netif_loopback(void);
unsigned netif_count(void);

/* route: loopback ip -> lo; local iface ip -> that iface; default */
struct netif *netif_route(uint32_t dst_ip);

/* ---- rx entry + timers -------------------------------------------------------------- */

/* full ethernet frame; copies anything it keeps before returning  */
void netif_input(const void *frame, unsigned len, void *arg);

/* stack-wide timers, called from housekeeping every ~2 ms          */
void net_timers_tick(uint64_t now_ms);

/* ---- arp ---------------------------------------------------------------------------- */

#define ARP_TABLE_MAX 8u

int      arp_lookup(uint32_t ip, uint8_t *hw_out);
/* resolves then sends via netif link_out; one queued frame max    */
int      arp_send_ip(struct netif *nif, uint32_t dst_ip,
                     const void *buf, unsigned len);
void     arp_tick(uint64_t now_ms);
unsigned arp_cache_entries(void);

/* ---- checksum -------------------------------------------------------------------------- */

uint16_t ip4_checksum(const void *buf, unsigned len);
uint16_t ip4_pseudo_checksum(uint32_t src, uint32_t dst,
                             uint8_t proto, const void *hdr_payload,
                             unsigned len);

/* rx entry points into the transport layers (net/udp.c, net/tcp.c) */
void udp_input(struct netif *nif, uint32_t src, uint32_t dst,
               const uint8_t *pkt, unsigned len);
void tcp_input(struct netif *nif, uint32_t src, uint32_t dst,
               const uint8_t *pkt, unsigned len);

/* ---- icmp ----------------------------------------------------------------------------- */

#define ICMP_ECHO_REPLY   0u
#define ICMP_ECHO_REQUEST 8u

void icmp_input(struct netif *nif, uint32_t src, uint32_t dst,
                const void *hdr, unsigned len);
/* blocking ping; returns rtt ms or negative errno                 */
int  icmp_ping(uint32_t dst, uint16_t id, uint16_t seq,
               uint32_t timeout_ms);

/* ---- udp ------------------------------------------------------------------------------- */

#define UDP_RING_SZ 2048u

struct udp_pcb {
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    uint8_t  ring[UDP_RING_SZ];
    uint16_t ring_lens[8];
    unsigned ring_head, ring_count;
    uint16_t dropped;

    /* socket linkage: sockets.c parks readers on this queue        */
    struct waitqueue *wq;
    void    *sock;

    struct udp_pcb *next;
};

struct udp_pcb *udp_alloc(void);
void udp_free(struct udp_pcb *pcb);
int  udp_bind(struct udp_pcb *pcb, uint32_t ip, uint16_t port);
int  udp_connect(struct udp_pcb *pcb, uint32_t ip, uint16_t port);
int  udp_send(struct udp_pcb *pcb, const void *buf, unsigned len);
int  udp_recv(struct udp_pcb *pcb, void *buf, unsigned buflen,
              bool wait);
unsigned udp_pending(const struct udp_pcb *pcb);

/* raw port hook used by dhcp/dns before sockets exist              */
typedef void (*udp_raw_fn)(uint32_t src_ip, uint16_t src_port,
                           const void *payload, unsigned len, void *arg);
int  udp_bind_raw(uint16_t port, udp_raw_fn fn, void *arg);


/* ---- tcp ------------------------------------------------------------------------------ */

#define TCP_MSS        1460u
#define TCP_RING_SZ    4096u
#define TCP_WINDOW     4096u
#define TCP_MAX_RETRIES 5u

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_pcb {
    enum tcp_state state;

    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    /* send side: single outstanding segment                        */
    uint32_t snd_una, snd_nxt;
    uint8_t  snd_seg[TCP_MSS];
    uint16_t snd_len;
    bool     snd_inflight;
    uint64_t snd_timeout_ms;
    unsigned retries;
    bool     fin_queued, fin_sent, fin_acked;
    bool     peer_fin, rst_received;

    /* receive side: in-order ring, app drains via sockets          */
    uint8_t  rcv_ring[TCP_RING_SZ];
    uint16_t rcv_head, rcv_count;
    bool     rcv_eof;

    uint32_t rcv_nxt;                   /* next expected seq          */
    uint32_t last_ack_sent;

    struct waitqueue *wq;               /* sockets park here          */
    void    *sock;

    /* listener backlog: accepted-but-unclaimed pcbs                */
    struct tcp_pcb *accepted[4];
    unsigned naccepted;

    struct tcp_pcb *next;
};

struct tcp_pcb *tcp_alloc(void);
void tcp_free(struct tcp_pcb *pcb);
struct tcp_pcb *tcp_next_pcb(struct tcp_pcb *cur);  /* table walk    */

int  tcp_bind(struct tcp_pcb *pcb, uint32_t ip, uint16_t port);
int  tcp_listen(struct tcp_pcb *pcb);
int  tcp_connect(struct tcp_pcb *pcb, uint32_t ip, uint16_t port,
                 uint32_t timeout_ms);
struct tcp_pcb *tcp_accept(struct tcp_pcb *listener,
                           uint32_t timeout_ms);

/* app IO; send may loop multiple segments; returns payload bytes  */
int  tcp_write(struct tcp_pcb *pcb, const void *buf, unsigned len);
int  tcp_output_pcb(struct tcp_pcb *pcb);
int  tcp_read(struct tcp_pcb *pcb, void *buf, unsigned len,
              bool wait);
int  tcp_close(struct tcp_pcb *pcb, uint32_t timeout_ms);

unsigned tcp_rcv_pending(const struct tcp_pcb *pcb);
bool     tcp_established(const struct tcp_pcb *pcb);
bool     tcp_eof(const struct tcp_pcb *pcb);

/* ---- dhcp / dns -------------------------------------------------------------------------- */

struct dhcp_result {
    bool     bound;
    uint32_t ip, netmask, gw, dns;
    uint32_t lease_sec;
};

int  dhcp_discover(struct netif *nif, struct dhcp_result *out,
                   uint32_t timeout_ms);
int  dns_resolve(const char *name, uint32_t *ip_out,
                 uint32_t timeout_ms);

/* ---- sockets (net/sockets.c; wired into syscalls.c) ------------------------------- */

long sys_socket(uint64_t domain, uint64_t type, uint64_t protocol);
long sys_connect(uint64_t fd, uint64_t addr_p, uint64_t len);
long sys_bind(uint64_t fd, uint64_t addr_p, uint64_t len);
long sys_listen(uint64_t fd, uint64_t backlog);
long sys_accept(uint64_t fd, uint64_t addr_p, uint64_t len_p);
long sys_send(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags);
long sys_recv(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags);
long sys_select(uint64_t nfds, uint64_t rd, uint64_t wr, uint64_t ex,
                uint64_t timeout_ms);

#endif /* NET_H */
