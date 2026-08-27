/*
 * virtio_input.c - legacy virtio-input event frontends (phase 9,
 * item 51). Covers QEMU's `-device virtio-tablet-device` (touch
 * axes) and `-device virtio-keyboard-device` (keys) over the same
 * mmio transport, feeding the evdev-style core with raw triples.
 *
 * Layout: RX slots pre-posted on the event queue, exactly like
 * virtio-net receive buffers; completions arrive from the transport
 * bottom half, parse into input_push() and re-arm immediately.
 *
 * Device config is read through the legacy config window:
 *   bytes 0..7  = {select, subsel, size, pad[5]}
 *   bytes 8..   = payload
 * A single 32-bit LE store latches both selector bytes; payloads are
 * pulled word-wise by virtio_config_read().
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "input.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mmio.h"
#include "task.h"
#include "time.h"
#include "virtio.h"

/* virtio-input command ABI                                        */
#define VINPUT_EVENTQ 0u

struct vin_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

#define VIN_SLOTS 8u

enum vin_role {
    VIN_TABLET = 0,
    VIN_KEYBOARD,
};

struct vin_slot {
    struct vin_event *buf;              /* uncached staging           */
    unsigned          head;             /* descriptor it occupies     */
};

struct vin_dev {
    struct virtio_dev *vt;
    struct virtq      *eventq;
    enum vin_role      role;

    bool    configured;
    int32_t ax_min, ax_max;             /* axis range (both axes)     */
    char    name[40];

    struct vin_slot slot[VIN_SLOTS];

    struct {
        uint64_t events, reposts;
    } stats;
};

static struct vin_dev *vtab;            /* first tablet               */
static struct vin_dev *vkey;            /* first keyboard             */

/* ---- config queries ------------------------------------------------------------ */

static void cfg_select(struct virtio_dev *d, uint8_t sel, uint8_t sub)
{
    /* one LE store: byte0 = select, byte1 = subsel                */
    mmio_write32(d->base + VREG_CONFIG,
                 (uint32_t)sel | ((uint32_t)sub << 8));
}

static uint32_t cfg_size(struct virtio_dev *d)
{
    /* byte2 of the same header block                              */
    return (virtio_reg_read(d, VREG_CONFIG) >> 16) & 0xffu;
}

#define CFG_ID_NAME   1u
#define CFG_SELECT_EV_MASK 1u        /* select=EV_* code bitmask    */
#define CFG_SELECT_ABS_INFO 2u

/* read absinfo {s32 min,s32 max} for an axis code                 */
static bool cfg_absinfo(struct virtio_dev *d, uint16_t axis,
                        int32_t *min_out, int32_t *max_out)
{
    uint8_t raw[8];

    cfg_select(d, CFG_SELECT_ABS_INFO, (uint8_t)axis);
    if (cfg_size(d) < sizeof(raw))
        return false;
    virtio_config_read(d, 8, raw, sizeof(raw));

    memcpy(min_out, &raw[0], 4);
    memcpy(max_out, &raw[4], 4);
    return true;
}

/* ---- rx path ------------------------------------------------------------------- */

static void vin_repost(struct vin_dev *v, struct vin_slot *s)
{
    struct virtq *q = v->eventq;
    unsigned h;

    h = virtq_pop_desc(q);
    if (h == VQ_NONE)
        return;                         /* ring full; slot dropped    */

    s->head = h;
    q->desc[h].addr  = virtio_stage_pa(s->buf);
    q->desc[h].len   = sizeof(*s->buf);
    q->desc[h].flags = VRING_DESC_F_WRITE;
    q->desc[h].next  = 0;

    virtq_submit(q, h, s);
    virtio_kick(v->vt, VINPUT_EVENTQ);
    v->stats.reposts++;
}

/* transport bottom half: deliver completed event buffers          */
static void vin_complete(struct virtio_dev *vt, struct virtq *q,
                         unsigned head, uint32_t len)
{
    struct vin_dev *v = vt->priv;
    struct vin_slot *s = NULL;

    (void)q;
    for (unsigned i = 0; i < VIN_SLOTS; i++)
        if (v->slot[i].head == head) {
            s = &v->slot[i];
            break;
        }
    if (!s || len < sizeof(struct vin_event))
        return;

    /*
     * Axes stream with their RAW device values: a window manager
     * maps them against the framebuffer once one exists (phase 15).
     * EV_SYN records pass through verbatim as batching markers.
     */
    input_push(s->buf->type, s->buf->code, (int32_t)s->buf->value);
    v->stats.events++;

    vin_repost(v, s);
}

static const char *role_name(enum vin_role r)
{
    return r == VIN_TABLET ? "ABS touch" : "keys";
}

static int vin_attach_one(struct virtio_dev *vt, enum vin_role role,
                          struct vin_dev **slot)
{
    struct vin_dev *v;
    uint8_t name_raw[64];
    bool tablet;

    if (*slot)                          /* one per role supported     */
        return -1;

    /* identify by reported device name before choosing the role   */
    cfg_select(vt, CFG_ID_NAME, 0);
    {
        uint32_t sz = cfg_size(vt);
        unsigned n = sz < sizeof(name_raw) - 1 ? sz
                                               : sizeof(name_raw) - 1;

        if (sz) {
            virtio_config_read(vt, 8, name_raw, n);
            name_raw[n] = 0;
        } else {
            name_raw[0] = 0;
        }
    }
    tablet = !!strstr((const char *)name_raw, "tablet") ||
             !!strstr((const char *)name_raw, "Tablet");
    if ((role == VIN_TABLET) != tablet)
        role = tablet ? VIN_TABLET : VIN_KEYBOARD;

    if (*slot)
        return -1;

    v = kzalloc(sizeof(*v));
    if (!v)
        return -1;
    v->vt   = vt;
    v->role = role;
    vt->priv    = v;
    vt->complete = vin_complete;

    virtio_guest_features(vt, 0);

    if (virtq_setup(vt, VINPUT_EVENTQ) != 0) {
        kprintf("vin: event queue setup failed\n");
        kfree(v);
        return -1;
    }
    v->eventq = &vt->vq[VINPUT_EVENTQ];

    memcpy(v->name, name_raw, sizeof(name_raw));

    if (role == VIN_TABLET &&
        cfg_absinfo(vt, ABS_X, &v->ax_min, &v->ax_max))
        kprintf("vin: axis range %d..%d\n",
                (int)v->ax_min, (int)v->ax_max);

    for (unsigned i = 0; i < VIN_SLOTS; i++) {
        v->slot[i].buf = virtio_stage_alloc(sizeof(struct vin_event));
        if (!v->slot[i].buf) {
            kprintf("vin: out of stage buffers\n");
            while (i--)
                ;                       /* arena never frees          */
            kfree(v);
            return -1;
        }
    }

    *slot = v;

    virtio_set_status(vt,
                      virtio_get_status(vt) | VS_DRIVER_OK);

    /* arm the pipeline now that DRIVER_OK is set                   */
    for (unsigned i = 0; i < VIN_SLOTS; i++)
        vin_repost(v, &v->slot[i]);

    kprintf("vin: %s online (%s)\n",
            v->name[0] ? v->name : "input", role_name(role));
    return 0;
}

int virtio_input_attach(struct virtio_dev *vt)
{
    struct vin_dev **slotp;
    bool tablet;
    uint8_t name_raw[64];

    cfg_select(vt, CFG_ID_NAME, 0);
    {
        uint32_t sz = cfg_size(vt);
        unsigned n = sz < sizeof(name_raw) - 1 ? sz
                                               : sizeof(name_raw) - 1;

        if (sz) {
            virtio_config_read(vt, 8, name_raw, n);
            name_raw[n] = 0;
        } else {
            name_raw[0] = 0;
        }
    }
    name_raw[sizeof(name_raw) - 1] = 0;

    tablet = !!strstr((const char *)name_raw, "tablet") ||
             !!strstr((const char *)name_raw, "Tablet");
    slotp = tablet ? &vtab : &vkey;

    return vin_attach_one(vt,
                          tablet ? VIN_TABLET : VIN_KEYBOARD,
                          slotp);
}
