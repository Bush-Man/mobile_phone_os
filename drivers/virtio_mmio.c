/*
 * virtio_mmio.c - legacy virtio-mmio transport + split-virtqueue core.
 *
 * QEMU `-M virt` wires up to 32 transports at 0x0a000000..0x0a003e00,
 * edge-triggered SPIs 16+n (GIC intid 48+n). Slots whose DeviceID
 * reads 0 are empty sockets and are left alone.
 *
 * Rings and staging buffers come from a static 4 KiB-aligned arena in
 * the kernel image (physically contiguous by construction -- the PFN
 * register demands contiguity) and are ALWAYS touched through the
 * uncached device-window alias, which keeps DMA coherent with zero
 * cache-maintenance instructions.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "device.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "mmio.h"
#include "panic.h"
#include "spinlock.h"
#include "tasklet.h"
#include "virtio.h"

/* ---- uncached DMA arena ---------------------------------------------------- */

#define ARENA_BYTES (128u * 1024u)

static uint8_t arena[ARENA_BYTES] __attribute__((aligned(4096)));
static uint64_t arena_next;             /* bump offset into arena */

void *virtio_dmap(uint64_t pa)
{
    return (void *)(uintptr_t)vmm_devmap(pa);
}

/* recover the physical address behind a dmap alias */
paddr_t virtio_unalias(const void *dmap_va)
{
    return (paddr_t)((uintptr_t)dmap_va - KERN_DEVICE_BASE);
}

static uint64_t arena_alloc(unsigned bytes, unsigned align)
{
    uint64_t off = (arena_next + align - 1) & ~((uint64_t)align - 1);

    if (off + bytes > ARENA_BYTES)
        panic("virtio: dma arena exhausted");
    arena_next = off + bytes;
    return (uint64_t)(uintptr_t)arena + off;
}

void *virtio_stage_alloc(unsigned bytes)
{
    return virtio_dmap(arena_alloc(bytes, 16));
}

paddr_t virtio_stage_pa(const void *stage_va)
{
    return virtio_unalias(stage_va);
}

/* ---- barriers ------------------------------------------------------------------ */

static inline void vmb(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

/* ---- register IO ----------------------------------------------------------------- */

void virtio_reg_write(struct virtio_dev *d, unsigned off, uint32_t v)
{
    mmio_write32(d->base + off, v);
}

uint32_t virtio_reg_read(struct virtio_dev *d, unsigned off)
{
    return mmio_read32(d->base + off);
}

uint32_t virtio_get_status(struct virtio_dev *d)
{
    return mmio_read32(d->base + VREG_STATUS);
}

uint32_t virtio_int_status(struct virtio_dev *d)
{
    return mmio_read32(d->base + VREG_INT_STATUS);
}

void virtio_set_status(struct virtio_dev *d, uint32_t st)
{
    mmio_write32(d->base + VREG_STATUS, st);
}

uint64_t virtio_host_features(struct virtio_dev *d)
{
    /* one 32-bit feature word in the legacy layout */
    return mmio_read32(d->base + VREG_HOST_FEATURES);
}

void virtio_guest_features(struct virtio_dev *d, uint64_t feats)
{
    mmio_write32(d->base + VREG_GUEST_FEATURES, (uint32_t)feats);
}

uint64_t virtio_config_read64(struct virtio_dev *d, unsigned off)
{
    uint64_t lo = mmio_read32(d->base + VREG_CONFIG + off);
    uint64_t hi = mmio_read32(d->base + VREG_CONFIG + off + 4);

    return lo | (hi << 32);
}

void virtio_config_read(struct virtio_dev *d, unsigned off,
                        void *dst, unsigned n)
{
    uint8_t *out = dst;

    for (unsigned i = 0; i < n; i += 4) {
        uint32_t w = mmio_read32(d->base + VREG_CONFIG + off + i);
        unsigned take = n - i < 4 ? n - i : 4;

        for (unsigned b = 0; b < take; b++)
            out[i + b] = (uint8_t)(w >> (8 * b));
    }
}

void virtio_kick(struct virtio_dev *d, unsigned qidx)
{
    vmb();
    mmio_write32(d->base + VREG_QUEUE_NOTIFY, qidx);
}

/* ---- virtqueue plumbing ------------------------------------------------------------ */

int virtq_setup(struct virtio_dev *d, unsigned qidx)
{
    struct virtq *q = &d->vq[qidx];
    uint32_t num_max;

    mmio_write32(d->base + VREG_QUEUE_SEL, qidx);
    num_max = mmio_read32(d->base + VREG_QUEUE_NUM_MAX);
    if (num_max == 0 || num_max == 0xffffffffu)
        return -1;

    mmio_write32(d->base + VREG_GUEST_PAGESIZE, 4096);  /* legacy req */
    mmio_write32(d->base + VREG_QUEUE_NUM,
                 VQ_NUM < num_max ? VQ_NUM : num_max);
    mmio_write32(d->base + VREG_QUEUE_ALIGN, 4096);

    q->pa = arena_alloc(VQ_RING_BYTES, 4096);

    /*
     * Legacy layout at align=4096:
     *   page 0: desc table (2048 B) then avail ring (~262 B)
     *   page 1: used ring (1030 B), aligned as the spec requires
     */
    q->desc  = virtio_dmap(q->pa);
    q->avail = virtio_dmap(q->pa + 16u * VQ_NUM);
    q->used  = virtio_dmap(ALIGN_UP(q->pa + 16u * VQ_NUM, 4096));

    /* free-list threading: each descriptor points at the next */
    for (unsigned i = 0; i < VQ_NUM; i++) {
        q->desc[i].flags = VRING_DESC_F_NEXT;
        q->desc[i].next  = (uint16_t)(i + 1);
        q->owner[i] = NULL;
    }
    q->desc[VQ_NUM - 1].flags = 0;
    q->free_head   = 0;
    q->free_count  = VQ_NUM;
    q->last_used   = 0;
    q->avail->flags = 0;                /* no used-event suppression */
    q->avail->idx   = 0;

    mmio_write32(d->base + VREG_QUEUE_PFN, (uint32_t)(q->pa >> 12));

    d->nvqs++;
    kprintf("virtio: dev %u queue %u: %u entries @ 0x%llx\n",
            d->devid, qidx, VQ_NUM, (unsigned long long)q->pa);
    return 0;
}

unsigned virtq_pop_desc(struct virtq *q)
{
    unsigned idx;

    if (q->free_count == 0)
        return VQ_NONE;

    idx = q->free_head;
    q->free_head = q->desc[idx].next;
    q->free_count--;
    return idx;
}

void virtq_submit(struct virtq *q, unsigned head, void *owner)
{
    uint16_t slot = q->avail->idx % VQ_NUM;

    q->owner[head] = owner;
    q->avail->ring[slot] = (uint16_t)head;

    vmb();
    q->avail->idx++;                    /* publish */
}

void virtq_free_chain(struct virtq *q, unsigned head)
{
    unsigned i = head;

    for (;;) {
        bool next_flag = q->desc[i].flags & VRING_DESC_F_NEXT;
        unsigned next = q->desc[i].next;

        q->desc[i].flags = next_flag ? VRING_DESC_F_NEXT : 0;
        q->desc[i].next  = q->free_head;
        q->free_head     = (uint16_t)i;
        q->free_count++;

        if (!next_flag)
            break;
        i = next;
    }
    q->owner[head] = NULL;
}

void virtq_drain_used(struct virtio_dev *d, struct virtq *q)
{
    vmb();
    while (q->last_used != q->used->idx) {
        volatile struct vring_used_elem *e =
            &q->used->ring[q->last_used % VQ_NUM];
        unsigned head = e->id;
        uint32_t len  = e->len;

        vmb();                          /* data before bookkeeping */
        if (d->complete)
            d->complete(d, q, head, len);
        virtq_free_chain(q, head);

        q->last_used++;
    }
}

/* ---- interrupt plumbing ----------------------------------------------------------------- */

static void vm_bottom(void *arg)
{
    struct virtio_dev *d = arg;

    for (unsigned i = 0; i < MAX_VQS; i++)
        if (d->vq[i].desc)
            virtq_drain_used(d, &d->vq[i]);
}

/*
 * Completion poll for the blocking request paths (vblk IO, gpu
 * commands). The used ring is the authoritative record of what the
 * device finished; the interrupt is only a hint that it moved. Any
 * waiter that sleeps on a ->done flag must call this so a lost or
 * mis-routed line cannot hang the request forever.
 */
void virtio_poll(struct virtio_dev *d)
{
    uint32_t st;

    if (!d)
        return;

    st = mmio_read32(d->base + VREG_INT_STATUS);
    if (st)
        mmio_write32(d->base + VREG_INT_ACK, st);

    vm_bottom(d);
}

static bool vm_irq_top(void *arg)
{
    struct virtio_dev *d = arg;
    uint32_t st = mmio_read32(d->base + VREG_INT_STATUS);

    if (!st)
        return false;                   /* not ours after all */

    mmio_write32(d->base + VREG_INT_ACK, st);
    tasklet_schedule(vm_bottom, d);
    return true;
}

/* ---- device model driver ------------------------------------------------------------------- */

static struct virtio_dev *devlist;
static spinlock_t list_lock = SPINLOCK_INIT;

static int vm_probe(struct device *dev)
{
    const struct resource *mmio = dev_resource(dev, RES_MMIO, 0);
    const struct resource *irqr = dev_resource(dev, RES_IRQ, 0);
    struct virtio_dev *d;
    uint32_t magic, version, devid;
    int r;

    if (!mmio || mmio->size < 0x200)
        return -1;

    magic   = mmio_read32(mmio->base + VREG_MAGIC);
    version = mmio_read32(mmio->base + VREG_VERSION);
    devid   = mmio_read32(mmio->base + VREG_DEVID);

    if (magic != VIRTIO_MAGIC)
        return -1;                      /* not a transport after all */

    if (devid == VDEV_ID_NONE) {
        /* empty socket: bind quietly so the log stays readable */
        dev->quiet_bind = true;
        return 0;
    }

    if (version != 1) {
        kprintf("virtio: %s: version %u unsupported (legacy only)\n",
                dev->name, version);
        return -1;
    }

    d = kzalloc(sizeof(*d));
    if (!d)
        return -1;
    d->base   = mmio->base;
    d->irq    = irqr ? (unsigned)irqr->base : 0;
    d->devid  = devid;
    d->vendor = mmio_read32(mmio->base + VREG_VENDOR);

    /* reset, then walk the status machine up to DRIVER-OK-ready */
    mmio_write32(d->base + VREG_STATUS, 0);
    mmio_write32(d->base + VREG_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

    switch (devid) {
    case VDEV_ID_BLK:
        r = virtio_blk_attach(d);
        break;
    case VDEV_ID_NET:
        r = virtio_net_attach(d);
        break;
    case VDEV_ID_GPU:
        r = virtio_gpu_attach(d);
        break;
    case VDEV_ID_INPUT:
        r = virtio_input_attach(d);
        break;
    default:
        kprintf("virtio: %s: devid %u has no frontend\n",
                dev->name, devid);
        r = -1;
        break;
    }

    if (r != 0) {
        mmio_write32(d->base + VREG_STATUS, VS_FAILED);
        kfree(d);
        return -1;
    }

    if (d->irq && irq_register(d->irq, "virtio-mmio", vm_irq_top, d)) {
        /*
         * virtio-mmio drives its interrupt line level-high and holds
         * it until InterruptACK clears InterruptStatus. Configuring
         * it edge-triggered loses every completion whose line was
         * already asserted, which stalls the queue-drain path.
         */
        irq_set_trigger_edge(d->irq, false);
        irq_set_priority(d->irq, 0x90);
        irq_enable(d->irq);
    } else {
        kprintf("virtio: %s running without interrupts\n", dev->name);
    }

    daif_state s = irq_local_save();
    spin_lock(&list_lock);
    d->next = devlist;
    devlist = d;
    spin_unlock(&list_lock);
    irq_local_restore(s);

    dev->priv = d;
    return 0;
}

struct driver virtio_mmio_drv = {
    .name         = "virtio-mmio",
    .bus          = &platform_bus,
    .compat_table = (const char *const[]){ "virtio,mmio", NULL },
    .probe        = vm_probe,
    .remove       = NULL,
};
