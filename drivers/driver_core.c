/*
 * driver_core.c - bus/device/driver model with FDT enumeration.
 *
 * Two passes over the flattened device tree:
 *   pass 1 collects phandles (+ each phandle node's #interrupt-cells)
 *   pass 2 creates struct device for every compatible-bearing node,
 *          parsing reg/interrupts against inherited cell widths.
 *
 * Drivers register per-bus compatible tables; device_probe_all()
 * binds and probes. Console UART is pre-claimed by early boot, so
 * enumeration skips it (see header).
 */

#include <stdint.h>
#include <stddef.h>

#include "device.h"
#include "fdt.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"

/* ---- local string helpers (freestanding) ---------------------------------- */

static unsigned long s_len(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    return n;
}

static int s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned long s_prefix_len_to_at(const char *s)
{
    unsigned long n = 0;

    while (s[n] && s[n] != '@')
        n++;
    return n;
}

/* ---- registries ------------------------------------------------------------ */

#define BUS_MAX 4

struct bus_type *buses[BUS_MAX];
unsigned nbuses;

struct bus_type platform_bus = { .name = "platform" };

void bus_register(struct bus_type *bus)
{
    if (!bus || nbuses >= BUS_MAX)
        return;
    for (unsigned i = 0; i < nbuses; i++)
        if (buses[i] == bus)
            return;                     /* already in */
    buses[nbuses++] = bus;
}

int driver_register(struct driver *drv)
{
    struct driver **p;

    if (!drv || !drv->bus || !drv->name || !drv->probe)
        return -1;

    p = &drv->bus->drivers;
    while (*p) {
        if (*p == drv)
            return 0;                   /* idempotent */
        p = &(*p)->next;
    }
    *p = drv;
    drv->next = NULL;
    return 0;
}

int device_register(struct device *dev)
{
    struct device **p;

    if (!dev || !dev->bus)
        return -1;

    p = &dev->bus->devices;
    while (*p)
        p = &(*p)->next;
    *p = dev;
    dev->next = NULL;
    return 0;
}

static bool subsys_ready;

void driver_subsys_init(void)
{
    if (subsys_ready)
        return;
    bus_register(&platform_bus);
    subsys_ready = true;
}

/* ---- stats ------------------------------------------------------------------ */

static unsigned nr_devices_created, nr_drivers_registered;
static unsigned nr_probes_ok, nr_probes_failed;

unsigned device_count(void)
{
    return nr_devices_created;
}

unsigned driver_count(void)
{
    return nr_drivers_registered;
}

const struct resource *dev_resource(const struct device *dev,
                                    enum res_type t, unsigned idx)
{
    unsigned seen = 0;

    for (int i = 0; i < dev->nres; i++) {
        if (dev->res[i].type != t)
            continue;
        if (seen++ == idx)
            return &dev->res[i];
    }
    return NULL;
}

/* ---- matching + lifecycle ---------------------------------------------------- */

bool driver_match(const struct driver *drv, const struct device *dev)
{
    if (!drv->compat_table)
        return false;

    for (int d = 0; d < dev->ncompat; d++)
        for (const char *const *c = drv->compat_table; *c; c++)
            if (s_eq(dev->compat[d], *c))
                return true;
    return false;
}

int device_probe_one(struct device *dev)
{
    if (dev->state != DEV_UNBOUND)
        return dev->state == DEV_BOUND ? 0 : -1;

    for (struct driver *d = dev->bus ? dev->bus->drivers : NULL; d;
         d = d->next) {
        if (!driver_match(d, dev))
            continue;

        dev->drv = d;
        if (d->probe(dev) == 0) {
            dev->state = DEV_BOUND;
            nr_probes_ok++;
            kprintf("drvcore: %s probed by %s\n", dev->name, d->name);
            return 0;
        }
        dev->drv = NULL;
        dev->state = DEV_FAILED;        /* matched but probe failed */
        nr_probes_failed++;
        return -1;
    }

    /* no driver matched: stays UNBOUND ("nodrv"), not an error */
    return 1;
}

int device_probe_all(void)
{
    int bound = 0;

    for (unsigned b = 0; b < nbuses; b++)
        for (struct device *dev = buses[b]->devices; dev; dev = dev->next)
            if (dev->state == DEV_UNBOUND && device_probe_one(dev) == 0)
                bound++;
    return bound;
}

void device_dump(void)
{
    static const char *st[] = { "nodrv", "bound", "failed" };

    for (unsigned b = 0; b < nbuses; b++)
        for (struct device *dev = buses[b]->devices; dev; dev = dev->next) {
            kprintf("drvcore: %-22s %-8s %-12s res %d\n",
                    dev->name,
                    st[dev->state],
                    dev->drv ? dev->drv->name : "-",
                    dev->nres);
        }
}

/* ---- FDT walker ---------------------------------------------------------------- */

#define PH_MAX 32

struct wctx {
    const struct fdt *f;
    struct { int off; uint32_t phandle; } ph[PH_MAX];
    unsigned nph;
    struct { int off; unsigned icells; }   ic[PH_MAX];
    unsigned nic;
};

uint64_t w_claimed_base;                 /* set by enumerate entry */

static unsigned long tok_byte_off(const struct fdt *f, const void *tok)
{
    return (unsigned long)((const uint8_t *)tok -
                           (const uint8_t *)f->hdr);
}

static const uint32_t *after_name(const uint32_t *tok, const char *name)
{
    return tok + 1 + (s_len(name) + 4) / 4;
}

static uint32_t be_cell(const void *p)
{
    return fdt_u32(p);
}

/* pass 1: harvest phandle -> (#interrupt-cells) facts */
static const uint32_t *pass1(struct wctx *w, const uint32_t *here)
{
    const struct fdt *f = w->f;
    int off = (int)tok_byte_off(f, here);
    const char *name = (const char *)(here + 1);
    const uint32_t *cur = after_name(here, name);

    for (;;) {
        switch (fdt_u32(cur)) {
        case FDT_NOP:
            cur++;
            break;

        case FDT_PROP: {
            uint32_t len     = fdt_u32(cur + 1);
            uint32_t nameoff = fdt_u32(cur + 2);
            const char *pname = f->strings + nameoff;

            if (len == 4) {
                uint32_t v = be_cell(cur + 3);

                if (s_eq(pname, "phandle") && w->nph < PH_MAX) {
                    w->ph[w->nph].off     = off;
                    w->ph[w->nph].phandle = v;
                    w->nph++;
                } else if (s_eq(pname, "#interrupt-cells") &&
                           w->nic < PH_MAX) {
                    w->ic[w->nic].off    = off;
                    w->ic[w->nic].icells = v;
                    w->nic++;
                }
            }
            cur += 3 + (len + 3) / 4;
            break;
        }

        case FDT_BEGIN_NODE:
            cur = pass1(w, cur);
            break;

        case FDT_END_NODE:
            return cur + 1;

        default:                        /* FDT_END / garbage */
            return cur + 1;
        }
    }
}

static unsigned lookup_icells(const struct wctx *w, uint32_t phandle)
{
    for (unsigned i = 0; i < w->nph; i++)
        if (w->ph[i].phandle == phandle) {
            int off = w->ph[i].off;

            for (unsigned j = 0; j < w->nic; j++)
                if (w->ic[j].off == off)
                    return w->ic[j].icells;
        }
    return 3;                           /* standard GIC binding */
}

/* non-device subtrees never become devices */
static const char *const skip_prefixes[] = {
    "memory", "chosen", "aliases", "cpus",
    "timer", "intc", "psci", "pmu",
};

static bool name_skipped(const char *name)
{
    unsigned long plen = s_prefix_len_to_at(name);

    for (unsigned i = 0; i < sizeof(skip_prefixes) /
                               sizeof(skip_prefixes[0]); i++) {
        const char *p = skip_prefixes[i];

        if (s_len(p) == plen) {
            unsigned long j = 0;

            while (j < plen && p[j] == name[j])
                j++;
            if (j == plen)
                return true;
        }
    }
    return false;
}

static void parse_reg(struct device *dev, const struct fdt *f, int node,
                      unsigned ac, unsigned sc)
{
    int len;
    const void *reg = fdt_getprop(f, node, "reg", &len);
    const uint32_t *c = reg;
    int words = len / 4;

    if (!reg)
        return;

    while (words >= (int)(ac + sc) && dev->nres < DEV_RES_MAX) {
        uint64_t base = 0, size = 0;

        for (unsigned i = 0; i < ac && i < 2; i++)
            base = (base << 32) | be_cell(c + i);
        for (unsigned i = 0; i < sc && i < 2; i++)
            size = (size << 32) | be_cell(c + ac + i);

        dev->res[dev->nres].type = RES_MMIO;
        dev->res[dev->nres].base = base;
        dev->res[dev->nres].size = size;
        dev->res[dev->nres].irq_type = 0;
        dev->res[dev->nres].irq_flags = 0;
        dev->nres++;

        c += ac + sc;
        words -= (int)(ac + sc);
    }
}

static void parse_irqs(struct device *dev, const struct wctx *w, int node)
{
    const struct fdt *f = w->f;
    int len;
    const void *ip = fdt_getprop(f, node, "interrupt-parent", &len);
    const void *irqs;
    const uint32_t *c;
    unsigned icells = 3;
    int words;

    if (ip && len == 4)
        icells = lookup_icells(w, be_cell(ip));

    irqs = fdt_getprop(f, node, "interrupts", &len);
    if (!irqs)
        return;

    c = irqs;
    words = len / 4;
    while (words >= (int)icells && dev->nres < DEV_RES_MAX) {
        uint32_t type  = be_cell(c);
        uint32_t num   = icells > 1 ? be_cell(c + 1) : 0;
        uint32_t flags = icells > 2 ? be_cell(c + 2) : 0;

        /* GIC binding: type 0 = SPI (+32), type 1 = PPI (+16) */
        uint32_t intid = (type == 0) ? num + IRQ_SPI_BASE
                       : (type == 1) ? num + IRQ_PPI_BASE
                                     : num;

        dev->res[dev->nres].type      = RES_IRQ;
        dev->res[dev->nres].base      = intid;
        dev->res[dev->nres].size      = 0;
        dev->res[dev->nres].irq_type  = type;
        dev->res[dev->nres].irq_flags = flags;
        dev->nres++;

        c += icells;
        words -= (int)icells;
    }
}

static void parse_compats(struct device *dev, const struct fdt *f, int node)
{
    int len;
    const char *blob = fdt_getprop(f, node, "compatible", &len);

    if (!blob)
        return;

    const char *s = blob;
    int left = len;

    while (left > 0 && dev->ncompat < DEV_COMPAT_MAX) {
        unsigned long sl = s_len(s);

        if (sl == 0)
            break;                      /* padded double-NUL */
        dev->compat[dev->ncompat++] = s;
        s += sl + 1;
        left -= (int)(sl + 1);
    }
}

/* pass 2: create devices; inherits #address/#size-cells downward */
static const uint32_t *pass2(struct wctx *w, const uint32_t *here,
                             unsigned ac, unsigned sc, unsigned depth)
{
    const struct fdt *f = w->f;
    const char *name = (const char *)(here + 1);
    int off = (int)tok_byte_off(f, here);
    const uint32_t *cur = after_name(here, name);
    int len;
    bool has_compat = fdt_getprop(f, off, "compatible", &len) != NULL;

    /*
     * A node's own #address/#size-cells describe its CHILDREN's reg,
     * so children must see updated values -- parse our own reg with
     * the inherited ones first, then refresh the context below.
     */
    if (has_compat && !name_skipped(name) &&
        nr_devices_created < DEVICE_MAX) {
        struct device *dev = kzalloc(sizeof(*dev));

        if (dev) {
            dev->name     = name;
            dev->bus      = &platform_bus;
            dev->fdt_node = off;
            dev->state    = DEV_UNBOUND;
            parse_compats(dev, f, off);
            parse_reg(dev, f, off, ac, sc);
            parse_irqs(dev, w, off);

            bool claimed = false;
            for (int i = 0; i < dev->nres && !claimed; i++)
                if (dev->res[i].type == RES_MMIO &&
                    dev->res[i].base == w_claimed_base)
                    claimed = true;

            if (claimed) {
                kfree(dev);             /* early-boot-owned console */
            } else if (device_register(dev) == 0) {
                nr_devices_created++;
            } else {
                kfree(dev);
            }
        }
    }

    /* children inherit refreshed cell widths */
    {
        const void *v;

        v = fdt_getprop(f, off, "#address-cells", &len);
        if (v && len == 4)
            ac = be_cell(v);
        v = fdt_getprop(f, off, "#size-cells", &len);
        if (v && len == 4)
            sc = be_cell(v);
    }

    for (;;) {
        switch (fdt_u32(cur)) {
        case FDT_NOP:
            cur++;
            break;

        case FDT_PROP: {
            uint32_t plen = fdt_u32(cur + 1);

            cur += 3 + (plen + 3) / 4;
            break;
        }

        case FDT_BEGIN_NODE:
            if (depth < 6)
                cur = pass2(w, cur, ac, sc, depth + 1);
            else
                cur = pass1(w, cur);    /* deep junk: just skip it */
            break;

        case FDT_END_NODE:
            return cur + 1;

        default:
            return cur + 1;
        }
    }
}

int devices_enumerate_fdt(const struct fdt *f, uint64_t claimed_mmio_base)
{
    struct wctx w;
    const uint32_t *cur;
    int root, len;
    unsigned ac = 2, sc = 2;
    const void *v;

    if (!f || !f->hdr)
        return -1;

    memset(&w, 0, sizeof(w));
    w.f = f;
    w_claimed_base = claimed_mmio_base;

    root = fdt_find_node(f, "/");
    if (root < 0)
        return -1;

    /* root cell widths (describe root's direct children) */
    v = fdt_getprop(f, root, "#address-cells", &len);
    if (v && len == 4)
        ac = be_cell(v);
    v = fdt_getprop(f, root, "#size-cells", &len);
    if (v && len == 4)
        sc = be_cell(v);

    cur = f->st;
    if (fdt_u32(cur) != FDT_BEGIN_NODE)
        return -1;

    cur = pass1(&w, cur);
    cur = pass2(&w, f->st, ac, sc, 0);
    (void)cur;
    return (int)nr_devices_created;
}
