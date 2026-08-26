/*
 * virtio_net.c - minimal legacy virtio-net frontend (phase 6 parity
 * groundwork for the phase-11 stack).
 *
 * Two queues: rx (0) and tx (1). RX buffers are pre-posted; each
 * chain is [10-byte net header (device-writable)][1514-byte frame].
 * Received frames are handed to a registered callback (or counted)
 * from the transport bottom half. TX copies the frame into an
 * uncached staging buffer and recycles on completion.
 *
 * QEMU exposes a NIC only when a backend is attached (-netdev ...);
 * with none, this driver simply never probes.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "task.h"
#include "time.h"
#include "virtio.h"

#define VNET_HDR_LEN      10u           /* legacy non-mergeable hdr */
#define VNET_FRAME_LEN    1514u
#define VNET_BUF_LEN      (VNET_HDR_LEN + VNET_FRAME_LEN)
#define VNET_RX_BUFFERS   8
#define VNET_FEATURE_MAC  (1ull << 5)
#define VNET_SLOT_FREE    0xffffu

struct vnet_rx_slot {
    uint8_t *buf;                       /* uncached staging */
    unsigned head;
};

struct vnet_dev {
    struct virtio_dev *vt;
    struct virtq *rx, *tx;

    uint8_t mac[6];

    struct vnet_rx_slot slot[VNET_RX_BUFFERS];

    /* single in-flight TX frame is plenty at this stage */
    uint8_t *tx_buf;
    volatile bool tx_busy;
    bool tx_ok;

    void (*rx_fn)(const void *frame, unsigned len, void *arg);
    void *rx_arg;

    struct {
        uint64_t rx_frames, tx_frames, rx_bytes, tx_bytes, drops;
    } stats;
};

static struct vnet_dev *vnet;           /* one NIC supported */

/* ---- RX ------------------------------------------------------------------------ */

/* slot buffers are allocated once at attach; this only arms descs */
static int vnet_post_rx(struct vnet_dev *n, unsigned i)
{
    struct virtq *q = n->rx;
    unsigned h, d1;
    struct vnet_rx_slot *s = &n->slot[i];

    h  = virtq_pop_desc(q);
    d1 = virtq_pop_desc(q);
    if (h == VQ_NONE || d1 == VQ_NONE)
        return -1;

    q->desc[h].addr  = virtio_stage_pa(s->buf);
    q->desc[h].len   = VNET_HDR_LEN;
    q->desc[h].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
    q->desc[h].next  = (uint16_t)d1;

    q->desc[d1].addr  = virtio_stage_pa(s->buf + VNET_HDR_LEN);
    q->desc[d1].len   = VNET_FRAME_LEN;
    q->desc[d1].flags = VRING_DESC_F_WRITE;
    q->desc[d1].next  = 0;

    s->head = h;
    return 0;
}

/* (re-)arm every free slot */
void vnet_refill(struct vnet_dev *n)
{
    bool kicked = false;

    for (unsigned i = 0; i < VNET_RX_BUFFERS; i++) {
        if (n->slot[i].head != VNET_SLOT_FREE)
            continue;
        if (vnet_post_rx(n, i) != 0)
            break;
        virtq_submit(n->rx, n->slot[i].head, &n->slot[i]);
        kicked = true;
    }
    if (kicked)
        virtio_kick(n->vt, 0);
}

/* ---- completion paths -------------------------------------------------------------- */

static void vnet_complete(struct virtio_dev *vt, struct virtq *q,
                          unsigned head, uint32_t len)
{
    struct vnet_dev *n = vt->priv;

    if (q == n->rx) {
        for (unsigned i = 0; i < VNET_RX_BUFFERS; i++) {
            struct vnet_rx_slot *s = &n->slot[i];

            if (s->head != head)
                continue;

            unsigned flen = len > VNET_HDR_LEN
                                ? len - VNET_HDR_LEN : 0;

            if (flen) {
                n->stats.rx_frames++;
                n->stats.rx_bytes += flen;
                if (n->rx_fn)
                    n->rx_fn(s->buf + VNET_HDR_LEN, flen, n->rx_arg);
            }

            s->head = VNET_SLOT_FREE;   /* descs freed by the drain */
            break;
        }
        return;
    }

    if (q == n->tx) {
        n->stats.tx_frames++;
        n->tx_busy = false;
    }
}

void virtio_net_poll(void)
{
    struct vnet_dev *n = vnet;

    if (!n)
        return;
    vnet_refill(n);
}

void virtio_net_set_rx_handler(void (*fn)(const void *, unsigned, void *),
                               void *arg)
{
    if (vnet) {
        vnet->rx_fn = fn;
        vnet->rx_arg = arg;
    }
}

/* ---- TX ----------------------------------------------------------------------------- */

int virtio_net_send(const void *frame, unsigned len)
{
    struct vnet_dev *n = vnet;
    struct virtq *q;
    unsigned h, d1;
    uint64_t deadline;

    if (!n || !frame || len == 0 || len > VNET_FRAME_LEN)
        return -1;
    if (n->tx_busy || !n->tx->desc)
        return -1;

    q = n->tx;
    memcpy(n->tx_buf, frame, len);      /* into uncached staging */

    h  = virtq_pop_desc(q);
    d1 = virtq_pop_desc(q);
    if (h == VQ_NONE || d1 == VQ_NONE)
        return -1;

    q->desc[h].addr  = virtio_stage_pa(n->tx_buf);
    q->desc[h].len   = VNET_HDR_LEN;    /* zeroed header = plain pkt */
    q->desc[h].flags = VRING_DESC_F_NEXT;
    q->desc[h].next  = (uint16_t)d1;

    q->desc[d1].addr  = virtio_stage_pa(n->tx_buf + VNET_HDR_LEN);
    q->desc[d1].len   = len;
    q->desc[d1].flags = 0;              /* device reads */
    q->desc[d1].next  = 0;

    n->tx_busy = true;
    virtq_submit(q, h, n);
    virtio_kick(n->vt, 1);

    deadline = jiffies_read() + TIME_HZ;        /* 1 s */
    while (n->tx_busy) {
        if ((long)(jiffies_read() - deadline) >= 0)
            return -1;
        msleep(2);
    }

    n->stats.tx_bytes += len;
    return 0;
}

const uint8_t *virtio_net_mac(void)
{
    return vnet ? vnet->mac : NULL;
}

bool virtio_net_present(void)
{
    return vnet != NULL;
}

/* ---- attach ---------------------------------------------------------------------------- */

int virtio_net_attach(struct virtio_dev *vt)
{
    struct vnet_dev *n;
    uint64_t feats;

    if (vnet)
        return -1;

    n = kzalloc(sizeof(*n));
    if (!n)
        return -1;

    n->vt = vt;
    vt->priv = n;
    vt->complete = vnet_complete;

    feats = virtio_host_features(vt);
    virtio_guest_features(vt, feats & VNET_FEATURE_MAC);

    virtio_config_read(vt, 0, n->mac, sizeof(n->mac));

    if (virtq_setup(vt, 0) != 0 || virtq_setup(vt, 1) != 0) {
        kprintf("vnet: queue setup failed\n");
        kfree(n);
        return -1;
    }
    n->rx = &vt->vq[0];
    n->tx = &vt->vq[1];

    for (unsigned i = 0; i < VNET_RX_BUFFERS; i++) {
        n->slot[i].buf = virtio_stage_alloc(VNET_BUF_LEN);
        n->slot[i].head = VNET_SLOT_FREE;
    }

    n->tx_buf = virtio_stage_alloc(VNET_BUF_LEN);

    vnet = n;
    virtio_set_status(vt, virtio_get_status(vt) | VS_DRIVER_OK);

    vnet_refill(n);                     /* arm the receive path */

    kprintf("vnet: nic %02x:%02x:%02x:%02x:%02x:%02x ready\n",
            n->mac[0], n->mac[1], n->mac[2],
            n->mac[3], n->mac[4], n->mac[5]);
    return 0;
}
