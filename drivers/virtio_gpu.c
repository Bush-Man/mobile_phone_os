/*
 * virtio_gpu.c - legacy virtio-gpu 2D frontend (phase 9, item 48).
 *
 * One control queue; the scanout presents a single guest-owned
 * XRGB8888 resource whose backing is a scatter list of ordinary
 * physical frames -- that is what lets us paint an ~1.9 MB canvas
 * without contiguous physical allocation, which neither the kernel
 * heap nor the PMM free-list could serve today. CPU draws happen
 * through uncached device-window aliases so host DMA sees them
 * coherently with zero cache maintenance.
 *
 * Boot-context attach() only negotiates and arms queues: every
 * command round-trip is deferred until a task context exists
 * (fb_claim_default from the graphics selftest), mirroring how vblk
 * defers sector IO.
 *
 * Double buffering: the QEMU head has no frame-completion events,
 * so this backend exposes single-scanout update-on-flush semantics;
 * flip() = transfer (device copies backing into the host surface)
 * followed by scanout setup + flush of the whole rect. The fb core's
 * flip() hook exists precisely so real panel backends (phase-10+
 * phone targets) can plug in true page flipping later.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "chardev.h"
#include "fb.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "panic.h"
#include "spinlock.h"
#include "sync.h"
#include "task.h"
#include "time.h"
#include "virtio.h"

/* ---- command ABI (LE fields, AArch64 native) ------------------------------- */

#define VG_CTRL_HDR_LEN   24u

#define VG_CMD_GET_DISPLAY_INFO      0x0110u
#define VG_CMD_RESOURCE_CREATE_2D    0x0101u
#define VG_CMD_RESOURCE_UNREF        0x0102u
#define VG_CMD_SET_SCANOUT           0x0103u
#define VG_CMD_RESOURCE_FLUSH        0x0104u
#define VG_CMD_TRANSFER_TO_HOST_2D   0x0105u
#define VG_CMD_RESOURCE_ATTACH_BACKING 0x0106u

#define VG_RESP_OK_NODATA            0x1100u
#define VG_RESP_ERR_UNSPEC           0x1200u

/*
 * Request stage sizing: ATTACH_BACKING carries one mem_entry (16 B)
 * per backing page. At 800x600x4 that is ~461 entries => ~7.4 KB
 * blob; a 16 KiB stage covers it comfortably (DMA arena headroom is
 * 128 KiB, ring pages take ~2 each).
 */
#define VG_REQ_STAGE_BYTES 16384u

/*
 * virtio_gpu_ctrl_hdr as the spec defines it: ctx_id and padding are
 * part of the header, so it is 24 bytes, not 16. Short-changing it
 * makes the device parse each payload 8 bytes early and reject
 * every command.
 */
struct vg_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct vg_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct vg_create2d {                    /* follows ctrl_hdr            */
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct vg_scanout {                     /* follows ctrl_hdr            */
    struct vg_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct vg_transfer {                    /* follows ctrl_hdr            */
    struct vg_rect r;
    uint64_t offset;
    uint32_t resource_id;
} __attribute__((packed));

struct vg_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct vg_attach_backing {              /* follows ctrl_hdr            */
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

/* ---- device state ------------------------------------------------------------------ */

#define VG_W 800u
#define VG_H 600u
/*
 * RESOURCE_CREATE_2D takes a virtio_gpu_formats enum value, NOT a DRM
 * fourcc -- passing FB_FMT_XRGB8888 ('XR24') here gets the command
 * rejected with RESP_ERR_INVALID_PARAMETER (0x1205). B8G8R8X8_UNORM
 * is the enum whose in-memory byte order matches our XRGB8888 canvas.
 */
#define VG_FMT_B8G8R8X8_UNORM 2u
#define VG_FMT VG_FMT_B8G8R8X8_UNORM

#define VG_TIMEOUT_TICKS 50             /* 500 ms at TIME_HZ           */

struct vgpu_state {
    struct virtio_dev *vt;
    struct virtq      *cq;

    struct kmutex lock;                 /* serializes ctrlq traffic    */

    struct {
        volatile bool done;
        bool          ok;
        uint32_t      resp_type;
    } active;

    uint8_t   *req_stage;               /* uncached, 4 KiB            */
    uint8_t   *resp_stage;              /* uncached, 64 B             */

    /* scanout resource: guest backing pages                       */
    unsigned    nframes;
    paddr_t    *frames;

    struct fb_canvas canvas;
    struct char_dev cd;                 /* "fb0"                      */
    bool scanout_armed;
    bool resources_ready;

    struct {
        uint64_t commands;
        uint64_t errors;
    } stats;
};

static struct vgpu_state *vgpu;         /* one head supported          */

/* ---- completion path (tasklet context) ---------------------------------------- */

static void vgpu_complete(struct virtio_dev *vt, struct virtq *q,
                          unsigned head, uint32_t len)
{
    struct vgpu_state *v = vt->priv;
    uint32_t resp_type = 0;

    (void)head;
    if (!v || q != v->cq)
        return;
    if (len >= 4)
        memcpy(&resp_type, v->resp_stage, 4);

    v->active.resp_type = resp_type;
    v->active.ok = (resp_type == VG_RESP_OK_NODATA);
    v->active.done = true;
}

/*
 * Push one control command: single readable blob (header + payload)
 * followed by one writable response descriptor. Serialized through
 * the backend mutex; completes via msleep-poll like vblk IO.
 */
static int gpu_cmd(uint32_t type, const void *payload, size_t plen)
{
    struct vgpu_state *v = vgpu;
    struct vg_ctrl_hdr hdr;
    unsigned h, d2;
    uint64_t deadline;
    int rc;

    if (!this_cpu()->current)
        panic("vgpu: command before entering the scheduler");

    kmutex_lock(&v->lock);

    memset(&hdr, 0, sizeof(hdr));
    hdr.type = type;

    memcpy(v->req_stage, &hdr, sizeof(hdr));
    if (plen && plen > VG_REQ_STAGE_BYTES - VG_CTRL_HDR_LEN)
        plen = VG_REQ_STAGE_BYTES - VG_CTRL_HDR_LEN;
    /*
     * payload == NULL means the caller already staged the body in
     * req_stage itself (attach_backing builds a variable-length
     * entry array there). Copying anyway read from address 0 and
     * zeroed that body, so the device saw resource_id 0.
     */
    if (plen && payload)
        memcpy(v->req_stage + VG_CTRL_HDR_LEN, payload, plen);
    memset(v->resp_stage, 0xff, 64);

    v->active.done = false;
    v->active.ok   = false;

    h  = virtq_pop_desc(v->cq);
    d2 = virtq_pop_desc(v->cq);
    if (h == VQ_NONE || d2 == VQ_NONE) {
        rc = -1;
        goto out_unlock;
    }

    v->cq->desc[h].addr  = virtio_stage_pa(v->req_stage);
    v->cq->desc[h].len   = (uint32_t)(VG_CTRL_HDR_LEN + plen);
    v->cq->desc[h].flags = VRING_DESC_F_NEXT;
    v->cq->desc[h].next  = (uint16_t)d2;

    v->cq->desc[d2].addr  = virtio_stage_pa(v->resp_stage);
    v->cq->desc[d2].len   = 64;
    v->cq->desc[d2].flags = VRING_DESC_F_WRITE;
    v->cq->desc[d2].next  = 0;

    virtq_submit(v->cq, h, &v->active);
    virtio_kick(v->vt, 0);

    deadline = jiffies_read() + VG_TIMEOUT_TICKS;
    while (!v->active.done) {
        if ((long)(jiffies_read() - deadline) >= 0) {
            v->stats.errors++;
            v->active.resp_type = 0;    /* nothing came back at all */
            kprintf("vgpu: command 0x%x timed out\n", type);
            rc = -1;
            goto out_unlock;
        }
        msleep(2);
        /* the used ring, not the interrupt, is the source of truth */
        virtio_poll(v->vt);
    }

    rc = v->active.ok ? 0 : -1;
    if (rc)
        v->stats.errors++;
    else
        v->stats.commands++;

out_unlock:
    kmutex_unlock(&v->lock);
    return rc;
}


/* ---- resources ------------------------------------------------------------------ */

/* allocate the scanout backing: nframes disjoint 4K pages          */
static int gpu_alloc_frames(struct vgpu_state *v)
{
    uint64_t bytes = (uint64_t)VG_W * VG_H * FB_BPP;
    unsigned need = (unsigned)((bytes + PAGE_SIZE - 1) >> PAGE_SHIFT);

    v->nframes = need;
    v->frames = kmalloc(need * sizeof(paddr_t));
    if (!v->frames) {
        v->nframes = 0;
        return -1;
    }

    for (unsigned i = 0; i < need; i++) {
        paddr_t fr = pmm_alloc();

        if (!fr) {
            while (i--)
                pmm_free(v->frames[i]);
            kfree(v->frames);
            v->frames = NULL;
            v->nframes = 0;
            return -1;
        }
        v->frames[i] = fr;
    }

    /* blank the canvas so the first transfer pushes known pixels   */
    for (unsigned i = 0; i < need; i++) {
        uint8_t *alias = (uint8_t *)(uintptr_t)vmm_dmap(v->frames[i]);

        memset(alias, 0, PAGE_SIZE);
    }
    return 0;
}

static int gpu_cmd_create_resource(void)
{
    struct vg_create2d body = {
        .resource_id = 1,
        .format      = VG_FMT,
        .width       = VG_W,
        .height      = VG_H,
    };

    return gpu_cmd(VG_CMD_RESOURCE_CREATE_2D, &body, sizeof(body));
}

static int gpu_cmd_attach_backing(void)
{
    struct vgpu_state *v = vgpu;
    struct vg_attach_backing hdr;
    struct vg_mem_entry *me;

    /*
     * Payload is resource_id THEN nr_entries, and only then the
     * entry array. Omitting resource_id both misdescribed the
     * command and left the array at an odd offset -- its 64-bit
     * addr stores then took an alignment fault, because the staging
     * buffer lives in the Device-nGnRE window where unaligned
     * accesses are architecturally forbidden.
     */
    hdr.resource_id = 1;
    hdr.nr_entries  = v->nframes;
    memcpy(v->req_stage + VG_CTRL_HDR_LEN, &hdr, sizeof(hdr));

    me = (struct vg_mem_entry *)(v->req_stage +
                                 VG_CTRL_HDR_LEN + sizeof(hdr));
    for (unsigned i = 0; i < hdr.nr_entries; i++) {
        me[i].addr    = v->frames[i];
        me[i].length  = PAGE_SIZE;
        me[i].padding = 0;
    }

    return gpu_cmd(VG_CMD_RESOURCE_ATTACH_BACKING, NULL,
                   sizeof(hdr) +
                   hdr.nr_entries * sizeof(struct vg_mem_entry));
}


static int gpu_set_scanout(void)
{
    struct vg_scanout body;

    memset(&body, 0, sizeof(body));
    body.r.width     = VG_W;
    body.r.height    = VG_H;
    body.scanout_id  = 0;
    body.resource_id = 1;
    return gpu_cmd(VG_CMD_SET_SCANOUT, &body, sizeof(body));
}

/*
 * Push the guest canvas into the host surface and present it:
 * TRANSFER_TO_HOST_2D copies backing->host surface (device reads the
 * mem_entry pages directly -- our uncached writes are already
 * coherent), then FLUSH makes that surface visible on scanout 0.
 * Also arms the scanout on first use.
 */
int fb_virtio_gpu_present(void)
{
    struct vgpu_state *v = vgpu;
    struct {
        struct vg_rect r;               /* full frame              */
        uint64_t offset;                /* into resource backing   */
        uint32_t rid;
    } xfer;
    struct {
        struct vg_rect r;
        uint32_t rid;
    } flush;
    int rc = -1;

    if (!v || !v->resources_ready || !this_cpu()->current)
        return -1;

    memset(&xfer, 0, sizeof(xfer));
    xfer.r.width  = VG_W;
    xfer.r.height = VG_H;
    xfer.rid      = 1;
    if (gpu_cmd(VG_CMD_TRANSFER_TO_HOST_2D, &xfer, sizeof(xfer)))
        goto out;

    if (!v->scanout_armed) {
        rc = gpu_set_scanout();
        if (rc)
            goto out;
    }

    memset(&flush, 0, sizeof(flush));
    flush.r.width = VG_W;
    flush.r.height = VG_H;
    flush.rid     = 1;
    rc = gpu_cmd(VG_CMD_RESOURCE_FLUSH, &flush, sizeof(flush));

    v->scanout_armed = true;
out:
    return rc;
}

/* ---- "fb0" chardev ----------------------------------------------------------------- */

static const char fb0_name[] = "fb0";

/*
 * Whole-canvas streaming copy in XRGB8888 scanline order. The fd-
 * backed mmap path (syscalls.c) is the fast lane for real drawing;
 * read/write exist so userspace can verify pixels without mmap.
 */
static int fb0_read(struct char_dev *cd, char *dst, unsigned max)
{
    struct vgpu_state *v;
    unsigned cap;

    if (!cd || !cd->priv)
        return 0;
    v = cd->priv;
    if (!v->resources_ready)
        return 0;                       /* canvas not armed yet       */
    cap = VG_W * VG_H * FB_BPP;
    if (max > cap)
        max = cap;

    {
        size_t done = 0;
        uint64_t off = 0;

        while (done < max) {
            uint64_t in_page = PAGE_SIZE -
                               ((off + done) & (PAGE_SIZE - 1));
            size_t chunk = (size_t)in_page;

            if ((uint64_t)chunk > max - done)
                chunk = max - done;
            memcpy(dst + done,
                   (uint8_t *)(uintptr_t)vmm_dmap(
                       v->frames[(off + done) >> PAGE_SHIFT] +
                       ((off + done) & (PAGE_SIZE - 1))),
                   chunk);
            done += chunk;
        }
    }
    return (int)max;
}

static int fb0_write(struct char_dev *cd, const char *src, unsigned n)
{
    struct vgpu_state *v;
    unsigned cap;

    if (!cd || !cd->priv || !src)
        return -1;
    v = cd->priv;
    if (!v->resources_ready)
        return -1;                      /* canvas not armed yet       */
    cap = VG_W * VG_H * FB_BPP;
    if (n > cap)
        n = cap;

    {
        size_t done = 0;
        uint64_t off = 0;

        while (done < n) {
            uint64_t in_page = PAGE_SIZE -
                               ((off + done) & (PAGE_SIZE - 1));
            size_t chunk = (size_t)in_page;

            if ((uint64_t)chunk > n - done)
                chunk = n - done;
            memcpy((uint8_t *)(uintptr_t)vmm_dmap(
                       v->frames[(off + done) >> PAGE_SHIFT] +
                       ((off + done) & (PAGE_SIZE - 1))),
                   src + done, chunk);
            done += chunk;
        }
    }
    return (int)n;
}


/* ---- claim path (first task-context use) ------------------------------------------- */

/*
 * Lazily arm the resources the first time fb_claim_default() runs
 * (guaranteed task context -- checked by gpu_cmd's panic guard).
 */
static int gpu_arm_resources(struct vgpu_state *v)
{
    int rc;

    if (v->resources_ready)
        return 0;
    if (gpu_alloc_frames(v)) {
        kprintf("vgpu: no frames for %ux%u canvas\n", VG_W, VG_H);
        return -1;
    }
    rc = gpu_cmd_create_resource();
    if (rc) {
        kprintf("vgpu: RESOURCE_CREATE_2D failed (resp 0x%x)\n",
                v->active.resp_type);
        return -1;
    }
    rc = gpu_cmd_attach_backing();
    if (rc) {
        kprintf("vgpu: ATTACH_BACKING failed (resp 0x%x)\n",
                v->active.resp_type);
        return -1;
    }
    v->resources_ready = true;
    kprintf("vgpu: canvas ready (%u backing pages)\n", v->nframes);
    return 0;
}

static int gpu_claim(struct fb_canvas *out)
{
    struct vgpu_state *v = vgpu;

    if (!v)
        return -1;
    if (gpu_arm_resources(v))
        return -1;

    memset(out, 0, sizeof(*out));
    out->name           = "virtgpu";
    out->width          = VG_W;
    out->height         = VG_H;
    out->stride_bytes   = VG_W * FB_BPP;
    out->nframes        = v->nframes;
    out->frames         = v->frames;
    out->double_buffered = false;       /* no fence events negotiated */
    out->flip           = NULL;
    out->priv           = v;
    return 0;
}

/*
 * Register the node at attach, not at claim: the compositor starts
 * long before gfxtest arms the canvas, and it only needs open() to
 * succeed -- the read/write/ioctl paths each check resources_ready
 * (or arm the canvas themselves) before touching a frame.
 */
static void fb0_node_register(struct vgpu_state *v)
{
    if (v->cd.name)
        return;
    v->cd.name  = fb0_name;
    v->cd.priv  = v;
    v->cd.read  = fb0_read;
    v->cd.write = fb0_write;
    if (char_dev_register(&v->cd))
        kprintf("vgpu: fb0 registration failed\n");
}

static const struct fb_backend gpu_backend = {
    .name     = "virtio-gpu",
    .priority = 10,
    .claim    = gpu_claim,
};

void fb_virtio_gpu_backend_register(void)
{
    fb_backend_register(&gpu_backend);
}

/* ---- attach (boot context; zero blocking, zero ring traffic) -------- */

int virtio_gpu_attach(struct virtio_dev *vt)
{
    static bool attached;
    struct vgpu_state *v;

    if (attached)
        return -1;                      /* single head supported      */

    v = kzalloc(sizeof(*v));
    if (!v)
        return -1;

    v->vt = vt;
    vt->priv    = v;
    vt->complete = vgpu_complete;

    virtio_guest_features(vt, 0);       /* no fences                  */

    if (virtq_setup(vt, 0) != 0) {      /* controlq only              */
        kprintf("vgpu: queue setup failed\n");
        kfree(v);
        return -1;
    }
    v->cq = &vt->vq[0];

    v->req_stage  = virtio_stage_alloc(VG_REQ_STAGE_BYTES);
    v->resp_stage = virtio_stage_alloc(64);
    if (!v->req_stage || !v->resp_stage) {
        kprintf("vgpu: no stage buffers\n");
        kfree(v);
        return -1;
    }

    kmutex_init(&v->lock, "vgpu");
    virtio_set_status(vt,
                      virtio_get_status(vt) | VS_DRIVER_OK);

    /* publish the head: gpu_claim/gpu_cmd/fb_virtio_gpu_present all
     * reach the device through this global, not through vt->priv */
    vgpu = v;

    fb0_node_register(v);               /* /dev/fb0 openable now      */

    attached = true;
    kprintf("vgpu: present (canvas arming deferred to first claim)\n");
    return 0;
}
