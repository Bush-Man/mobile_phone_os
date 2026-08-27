#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

#include "mm/types.h"

/*
 * Legacy virtio-mmio transport (register layout version 1 -- what
 * QEMU's `-M virt` instantiates by default) plus split-virtqueue
 * helpers shared by the blk/net frontends.
 *
 * All CPU-side ring/buffer accesses go through the kernel's uncached
 * device window so device DMA stays coherent without any cache
 * maintenance instructions (same trick SMP bring-up uses).
 */

#define VIRTIO_MAGIC 0x74726976u        /* "virt" */

/* legacy (version==1) register offsets */
#define VREG_MAGIC          0x000
#define VREG_VERSION        0x004
#define VREG_DEVID          0x008
#define VREG_VENDOR         0x00c
#define VREG_HOST_FEATURES  0x010
#define VREG_HOST_FSEL      0x014
#define VREG_GUEST_FEATURES 0x020
#define VREG_GUEST_FSEL     0x024
#define VREG_GUEST_PAGESIZE 0x028
#define VREG_QUEUE_SEL      0x030
#define VREG_QUEUE_NUM_MAX  0x034
#define VREG_QUEUE_NUM      0x038
#define VREG_QUEUE_ALIGN    0x03c
#define VREG_QUEUE_PFN      0x040
#define VREG_QUEUE_NOTIFY   0x044
#define VREG_INT_STATUS     0x050
#define VREG_INT_ACK        0x054
#define VREG_STATUS         0x060
#define VREG_CONFIG         0x100

#define VDEV_ID_NONE 0u
#define VDEV_ID_NET  1u
#define VDEV_ID_BLK  2u
/* phase 9 frontends */
#define VDEV_ID_GPU   16u
#define VDEV_ID_INPUT 18u

/* status register bits */
#define VS_ACKNOWLEDGE  (1u << 0)
#define VS_DRIVER       (1u << 1)
#define VS_DRIVER_OK    (1u << 4)
#define VS_FAILED       (1u << 7)

/* split virtqueue descriptor flags */
#define VRING_DESC_F_NEXT   1u
#define VRING_DESC_F_WRITE  2u

#define VQ_NUM 128                      /* our fixed queue depth */
#define VQ_RING_BYTES (2u * 4096u)      /* desc+avail page, used page */
#define VQ_NONE   0xffffu               /* no descriptor available */

/* ---- rings ------------------------------------------------------------------ */

struct vring_desc {
    volatile uint64_t addr;
    volatile uint32_t len;
    volatile uint16_t flags;
    volatile uint16_t next;
};

struct vring_avail {
    volatile uint16_t flags;
    volatile uint16_t idx;
    volatile uint16_t ring[VQ_NUM];
};

struct vring_used_elem {
    volatile uint32_t id;               /* head descriptor index */
    volatile uint32_t len;              /* bytes the device wrote */
};

struct vring_used {
    volatile uint16_t flags;
    volatile uint16_t idx;
    volatile struct vring_used_elem ring[VQ_NUM];
};

struct virtq {
    uint64_t pa;                        /* physical base of 2-page span */
    struct vring_desc  *desc;           /* all pointers are dmap aliases */
    struct vring_avail *avail;
    struct vring_used  *used;

    uint16_t last_used;                 /* drained up to here            */
    uint16_t free_head;                 /* first free descriptor         */
    uint16_t free_count;
    void *owner[VQ_NUM];                /* chain-owner per head desc     */
};

/* ---- device -------------------------------------------------------------------- */

#define MAX_VQS 2

struct virtio_dev {
    uintptr_t base;                     /* MMIO base                     */
    unsigned irq;                       /* GIC intid                     */
    uint32_t devid;
    uint32_t vendor;
    struct virtq vq[MAX_VQS];
    unsigned nvqs;

    void (*complete)(struct virtio_dev *, struct virtq *,
                     unsigned head, uint32_t len);

    void *priv;
    struct virtio_dev *next;
};

/* ---- transport ops (implemented in virtio_mmio.c) -------------------------------- */

void     virtio_reg_write(struct virtio_dev *d, unsigned off, uint32_t v);
uint32_t virtio_reg_read(struct virtio_dev *d, unsigned off);

uint32_t virtio_get_status(struct virtio_dev *d);
void     virtio_set_status(struct virtio_dev *d, uint32_t st);
uint64_t virtio_host_features(struct virtio_dev *d);
void     virtio_guest_features(struct virtio_dev *d, uint64_t feats);
uint64_t virtio_config_read64(struct virtio_dev *d, unsigned off);
void     virtio_config_read(struct virtio_dev *d, unsigned off,
                            void *dst, unsigned n);
void     virtio_kick(struct virtio_dev *d, unsigned qidx);

int  virtq_setup(struct virtio_dev *d, unsigned qidx);  /* alloc + attach */
unsigned virtq_pop_desc(struct virtq *q);               /* VQ_NONE if full */
void virtq_free_chain(struct virtq *q, unsigned head);
void virtq_submit(struct virtq *q, unsigned head, void *owner);
void virtq_drain_used(struct virtio_dev *d, struct virtq *q);

/* staging buffers from the uncached arena (DMA-coherent by aliasing) */
void *virtio_stage_alloc(unsigned bytes);
paddr_t virtio_stage_pa(const void *stage_va);

/* ---- frontends (probed by the transport after devid detection) ---------------------- */

int virtio_blk_attach(struct virtio_dev *d);    /* drivers/virtio_blk.c */
int virtio_net_attach(struct virtio_dev *d);    /* drivers/virtio_net.c */
int virtio_gpu_attach(struct virtio_dev *d);    /* drivers/virtio_gpu.c (phase 9) */

/* ---- virtio-net API (phase 6 report path; stack lands in phase 11) ------------------- */

bool virtio_net_present(void);
const uint8_t *virtio_net_mac(void);
int  virtio_net_send(const void *frame, unsigned len);
void virtio_net_set_rx_handler(void (*fn)(const void *frame, unsigned len,
                                          void *arg), void *arg);
void virtio_net_poll(void);

#endif /* VIRTIO_H */
