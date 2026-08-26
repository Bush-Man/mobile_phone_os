/*
 * virtio_blk.c - legacy virtio-blk frontend.
 *
 * Request layout per spec: 16-byte header {type, resv, sector}, data
 * buffer, 1-byte status byte -- a three-descriptor chain. Completion
 * runs from the transport bottom half; the blocking wrapper polls
 * the done flag with msleep(), which keeps it safe from task context.
 *
 * All DMA buffers live in the uncached arena: writes are copied in
 * before submission, reads are copied out after completion, so no
 * cache maintenance instructions ever run.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "block.h"
#include "lib.h"
#include "mm/kheap.h"
#include "task.h"
#include "time.h"
#include "virtio.h"

#define VBLK_REQ_IN    0u
#define VBLK_REQ_OUT   1u

#define VBLK_STATUS_OK 0u
#define VBLK_TIMEOUT_TICKS (3u * TIME_HZ)

#define VBLK_MAX_SECTORS 8

struct vblk_hdr {
    uint32_t type;
    uint32_t resv;
    uint64_t sector;
};

struct vblk_req {
    volatile bool done;
    bool ok;
};

struct vblk_dev {
    struct block_device bd;
    struct virtio_dev *vt;
    struct virtq *q;

    /* single outstanding request; blocking callers serialize */
    struct vblk_req active;

    /* uncached staging */
    struct vblk_hdr *st_hdr;
    uint8_t *st_data;
    uint8_t *st_status;

    char name[BLK_NAME_MAX];

    struct {
        uint64_t reads, writes, errors;
    } stats;
};

/* ---- completion ------------------------------------------------------------------ */

static void vblk_complete(struct virtio_dev *vt, struct virtq *q,
                          unsigned head, uint32_t len)
{
    struct vblk_dev *v = vt->priv;
    struct vblk_req *req = q->owner[head];

    (void)len;
    if (!v || req != &v->active)
        return;

    req->ok   = (*v->st_status == VBLK_STATUS_OK);
    req->done = true;
}

/* ---- request path -------------------------------------------------------------------- */

static int vblk_rw(struct vblk_dev *v, bool write, uint64_t lba,
                   unsigned nsect, void *buf)
{
    struct virtq *q = v->q;
    struct vblk_req *req = &v->active;
    unsigned h, d1, d2;
    uint64_t deadline;
    int rc = -1;

    if (!req->done)
        return -1;                      /* previous request outstanding */

    if (write)
        memcpy(v->st_data, buf, nsect * BLK_SECTOR_SIZE);

    v->st_hdr->type   = write ? VBLK_REQ_OUT : VBLK_REQ_IN;
    v->st_hdr->resv   = 0;
    v->st_hdr->sector = lba;
    *v->st_status     = 0xffu;

    req->done = false;
    req->ok   = false;

    h  = virtq_pop_desc(q);
    d1 = virtq_pop_desc(q);
    d2 = virtq_pop_desc(q);
    if (h == VQ_NONE || d1 == VQ_NONE || d2 == VQ_NONE)
        goto out_restore;

    /* hdr: device reads it */
    q->desc[h].addr  = virtio_stage_pa(v->st_hdr);
    q->desc[h].len   = sizeof(*v->st_hdr);
    q->desc[h].flags = VRING_DESC_F_NEXT;
    q->desc[h].next  = (uint16_t)d1;

    /* data: direction flips with the operation */
    q->desc[d1].addr  = virtio_stage_pa(v->st_data);
    q->desc[d1].len   = nsect * BLK_SECTOR_SIZE;
    q->desc[d1].flags = write ? 0 : VRING_DESC_F_WRITE;
    q->desc[d1].next  = (uint16_t)d2;

    /* status byte: last writable link, terminates the chain */
    q->desc[d2].addr  = virtio_stage_pa(v->st_status);
    q->desc[d2].len   = 1;
    q->desc[d2].flags = VRING_DESC_F_WRITE;
    q->desc[d2].next  = 0;

    virtq_submit(q, h, req);
    virtio_kick(v->vt, 0);

    deadline = jiffies_read() + VBLK_TIMEOUT_TICKS;
    while (!req->done) {
        if ((long)(jiffies_read() - deadline) >= 0) {
            v->stats.errors++;
            goto out_restore;
        }
        msleep(2);
    }

    if (req->ok) {
        if (!write)
            memcpy(buf, v->st_data, nsect * BLK_SECTOR_SIZE);
        rc = 0;
    } else {
        v->stats.errors++;
    }

out_restore:
    return rc;
}

static int vblk_read(struct block_device *bd, uint64_t lba,
                     void *buf, unsigned nsect)
{
    struct vblk_dev *v = bd->priv;

    if (nsect > VBLK_MAX_SECTORS || lba + nsect > bd->capacity_sectors)
        return -1;
    if (vblk_rw(v, false, lba, nsect, buf) == 0) {
        v->stats.reads += nsect;
        return 0;
    }
    return -1;
}

static int vblk_write(struct block_device *bd, uint64_t lba,
                      const void *buf, unsigned nsect)
{
    struct vblk_dev *v = bd->priv;

    if (nsect > VBLK_MAX_SECTORS || lba + nsect > bd->capacity_sectors)
        return -1;
    if (vblk_rw(v, true, lba, nsect, (void *)buf) == 0) {
        v->stats.writes += nsect;
        return 0;
    }
    return -1;
}

/* ---- attach ------------------------------------------------------------------------------ */

int virtio_blk_attach(struct virtio_dev *vt)
{
    static bool attached;
    struct vblk_dev *v;
    uint64_t capacity;

    if (attached)
        return -1;                      /* one disk supported */

    v = kzalloc(sizeof(*v));
    if (!v)
        return -1;

    v->vt = vt;
    vt->priv    = v;
    vt->complete = vblk_complete;

    /* legacy negotiation: no feature bits required */
    virtio_guest_features(vt, 0);

    if (virtq_setup(vt, 0) != 0) {
        kprintf("vblk: queue setup failed\n");
        kfree(v);
        return -1;
    }
    v->q = &vt->vq[0];

    capacity = virtio_config_read64(vt, 0);     /* LE64 @ config+0 */
    if (capacity == 0) {
        kprintf("vblk: zero-capacity disk\n");
        kfree(v);
        return -1;
    }

    v->st_hdr    = virtio_stage_alloc(sizeof(*v->st_hdr));
    v->st_data   = virtio_stage_alloc(VBLK_MAX_SECTORS * 512);
    v->st_status = virtio_stage_alloc(1);
    v->active.done = true;

    static const char name[] = "vblk0";         /* bd.name points here */
    v->name[0] = '\0';
    for (unsigned i = 0; i < sizeof(name); i++)
        v->name[i] = name[i];

    v->bd.name             = v->name;
    v->bd.priv             = v;
    v->bd.capacity_sectors = capacity;
    v->bd.max_sectors      = VBLK_MAX_SECTORS;
    v->bd.read_blocks      = vblk_read;
    v->bd.write_blocks     = vblk_write;

    virtio_set_status(vt, virtio_get_status(vt) | VS_DRIVER_OK);

    if (block_register(&v->bd) != 0) {
        kfree(v);
        return -1;
    }

    attached = true;
    return 0;
}
