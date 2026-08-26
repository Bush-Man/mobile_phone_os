#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Phase 6 driver model: bus / device / driver triangle.
 *
 * - Devices are created from the flattened device tree (any node with
 *   a "compatible" property), carrying MMIO ranges and IRQ lines as
 *   parsed resources.
 * - Drivers register a NULL-terminated compatible-string table; the
 *   core matches strings exactly, binds the first match and calls
 *   probe(). probe() returns 0 on success (device becomes BOUND).
 * - remove() reverses a successful bind (used on shutdown paths).
 *
 * Device names and compatible strings point straight into the linked
 * DTB image (.rodata.fdt), which lives forever -- no copies needed.
 */

#define DEV_COMPAT_MAX  8       /* compatible strings kept per device */
#define DEV_RES_MAX     8       /* MMIO ranges + IRQ lines            */
#define DEVICE_MAX      64      /* hard cap on enumerated devices     */
#define DRIVER_NAME_MAX 24

enum res_type {
    RES_NONE = 0,
    RES_MMIO,
    RES_IRQ,
};

/* one MMIO window or one interrupt line */
struct resource {
    enum res_type type;
    uint64_t base;              /* MMIO: physical address. IRQ: GIC intid */
    uint64_t size;             /* MMIO: length; unused for IRQ           */
    unsigned irq_type;         /* IRQ: DT binding type cell              */
    unsigned irq_flags;        /* IRQ: DT binding flags (edge/level)     */
};

enum dev_state {
    DEV_UNBOUND = 0,            /* created, waiting for a matching driver */
    DEV_BOUND,                  /* some driver's probe() succeeded        */
    DEV_FAILED,                 /* probe() ran and failed                 */
};

struct device {
    const char *name;                       /* unit name "pl011@9000000" */
    const char *compat[DEV_COMPAT_MAX];     /* into the DTB blob         */
    int ncompat;

    struct resource res[DEV_RES_MAX];
    int nres;

    struct bus_type *bus;
    struct driver   *drv;                   /* NULL while unbound        */
    enum dev_state state;
    void *priv;                             /* driver-private data       */
    int fdt_node;                           /* DTB offset, or -1         */

    struct device *next;                    /* per-bus device list       */
};

struct driver {
    const char *name;
    struct bus_type *bus;
    const char *const *compat_table;        /* NULL-terminated           */
    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);

    struct driver *next;                    /* per-bus driver list       */
};

struct bus_type {
    const char *name;
    struct device *devices;                 /* list head                 */
    struct driver *drivers;
};

/* ---- registries --------------------------------------------------------- */

void bus_register(struct bus_type *bus);
int  driver_register(struct driver *drv);   /* returns 0 or -E-ish (-1)  */
int  device_register(struct device *dev);
void driver_subsys_init(void);              /* registers built-in buses  */

extern struct bus_type platform_bus;        /* FDT-enumerated devices    */

/* ---- resource access helper ---------------------------------------------- */

const struct resource *dev_resource(const struct device *dev,
                                    enum res_type t, unsigned idx);

/* ---- matching + lifecycle ------------------------------------------------- */

bool driver_match(const struct driver *drv, const struct device *dev);
int  device_probe_all(void);                /* attach every unbound dev  */
int  device_probe_one(struct device *dev);  /* single-device attach      */

/* ---- FDT enumeration ------------------------------------------------------- */

struct fdt;

/*
 * Walks the tree creating devices for every node with a "compatible"
 * property. Nodes whose name starts with a known non-device prefix
 * (memory, cpus, chosen, ...) are skipped, and so is any node whose
 * first MMIO base equals claimed_mmio_base (the early-boot console
 * UART is owned by drivers/uart.c long before this runs). Returns
 * the number of devices created.
 */
int devices_enumerate_fdt(const struct fdt *f, uint64_t claimed_mmio_base);

/* ---- debug ------------------------------------------------------------------ */

void device_dump(void);                     /* one line per device       */
unsigned device_count(void);
unsigned driver_count(void);

#endif /* DEVICE_H */
