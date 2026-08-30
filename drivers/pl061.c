/*
 * pl061.c - ARM PrimeCell PL061 GPIO controller driver.
 *
 * QEMU virt maps one at 0x09030000 with its interrupt on SPI 7.
 * Register notes that matter:
 *   - GPIODATA uses masked addressing: bits [9:2] of the bus address
 *     select which pins the access touches; within the register pin n
 *     is simply bit n.
 *   - Direction is per-pin through GPIODIR (1 = output).
 *   - Interrupt block supports level/edge per pin; we arm rising-edge.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "device.h"
#include "gpio.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mmio.h"
#include "spinlock.h"

#define PL061_DATA_BASE 0x000u
#define PL061_DIR       0x400u
#define PL061_IS        0x404u
#define PL061_IBE       0x408u
#define PL061_IEV       0x40cu
#define PL061_IE        0x410u
#define PL061_RIS       0x414u
#define PL061_MIS       0x418u
#define PL061_IC        0x41cu

#define PL061_NPINS 8

struct pl061 {
    uintptr_t base;
    int irq_intid;              /* -1 when the DTB gives us none */
    bool irq_armed;
    spinlock_t lock;
};

static uint32_t pl_data_mask(unsigned off)
{
    return (1u << off) << 2;    /* pin -> address bits [9:2] */
}

static int pl_dir_out(struct gpio_chip *c, unsigned off, bool val)
{
    struct pl061 *p = c->priv;
    uint32_t bit = 1u << off;

    spin_lock(&p->lock);
    mmio_write32(p->base + PL061_DIR,
                 mmio_read32(p->base + PL061_DIR) | bit);
    /* masked-data write touches exactly this pin */
    mmio_write32(p->base + PL061_DATA_BASE + pl_data_mask(off),
                 val ? bit : 0u);
    spin_unlock(&p->lock);
    return 0;
}

static int pl_dir_in(struct gpio_chip *c, unsigned off)
{
    struct pl061 *p = c->priv;
    uint32_t bit = 1u << off;

    spin_lock(&p->lock);
    mmio_write32(p->base + PL061_DIR,
                 mmio_read32(p->base + PL061_DIR) & ~bit);
    spin_unlock(&p->lock);
    return 0;
}

static void pl_set(struct gpio_chip *c, unsigned off, bool val)
{
    struct pl061 *p = c->priv;

    /* masked addressing makes this atomic against other pins */
    mmio_write32(p->base + PL061_DATA_BASE + pl_data_mask(off),
                 val ? (1u << off) : 0u);
}

static int pl_get(struct gpio_chip *c, unsigned off)
{
    struct pl061 *p = c->priv;
    uint32_t v;

    v = mmio_read32(p->base + PL061_DATA_BASE + pl_data_mask(off));
    return (int)((v >> off) & 1u);
}

/* ---- pin interrupts --------------------------------------------------------- */

static int pl_irq_enable(struct gpio_chip *c, unsigned off, bool on)
{
    struct pl061 *p = c->priv;
    uint32_t bit = 1u << off;
    daif_state s;

    if (!p->irq_armed)
        return -1;              /* controller line never registered */

    s = irq_local_save();
    spin_lock(&p->lock);
    /* edge-sensitive, rising */
    mmio_write32(p->base + PL061_IS,  mmio_read32(p->base + PL061_IS) & ~bit);
    mmio_write32(p->base + PL061_IBE, mmio_read32(p->base + PL061_IBE) & ~bit);
    mmio_write32(p->base + PL061_IEV, mmio_read32(p->base + PL061_IEV) | bit);
    mmio_write32(p->base + PL061_IC, bit);
    {
        uint32_t ie = mmio_read32(p->base + PL061_IE);

        mmio_write32(p->base + PL061_IE, on ? (ie | bit) : (ie & ~bit));
    }
    spin_unlock(&p->lock);
    irq_local_restore(s);
    return 0;
}

static bool pl061_irq_top(void *arg)
{
    struct gpio_chip *chip = arg;
    struct pl061 *p = chip->priv;
    uint32_t mis = mmio_read32(p->base + PL061_MIS);

    if (!mis)
        return false;           /* not ours after all */

    mmio_write32(p->base + PL061_IC, mis);      /* ack at controller */
    gpio_irq_dispatch(chip, mis);
    return true;
}

/* ---- driver model glue -------------------------------------------------------- */

static struct gpio_chip pl061_chip;     /* registry links it permanently */
static bool pl061_present;

static int pl_probe(struct device *dev)
{
    const struct resource *mmio = dev_resource(dev, RES_MMIO, 0);
    const struct resource *irqr = dev_resource(dev, RES_IRQ, 0);
    struct pl061 *p;

    if (pl061_present)
        return -1;              /* one controller supported */

    if (!mmio || mmio->size < 0x420)
        return -1;

    p = kzalloc(sizeof(*p));
    if (!p)
        return -1;
    p->base = mmio->base;
    p->irq_intid = irqr ? (int)irqr->base : -1;
    p->lock = (spinlock_t)SPINLOCK_INIT;

    /* mask every pin interrupt until somebody arms one */
    mmio_write32(p->base + PL061_IE, 0);
    mmio_write32(p->base + PL061_IC, 0xffu);

    pl061_chip.label = "pl061";
    pl061_chip.base  = 0;       /* first controller owns global 0..7 */
    pl061_chip.ngpio = PL061_NPINS;
    pl061_chip.priv  = p;
    pl061_chip.dir_out = pl_dir_out;
    pl061_chip.dir_in  = pl_dir_in;
    pl061_chip.set     = pl_set;
    pl061_chip.get     = pl_get;
    pl061_chip.irq_enable = pl_irq_enable;

    if (gpio_chip_register(&pl061_chip) != 0) {
        kfree(p);
        return -1;
    }

    if (p->irq_intid >= 0) {
        if (irq_register((unsigned)p->irq_intid, "pl061",
                         pl061_irq_top, &pl061_chip)) {
            p->irq_armed = true;
            irq_set_priority((unsigned)p->irq_intid, 0xa0);
            irq_enable((unsigned)p->irq_intid);
        } else {
            kprintf("pl061: irq %d busy, pin irqs disabled\n",
                    p->irq_intid);
        }
    }

    pl061_present = true;
    dev->priv = p;
    return 0;
}

static void pl_remove(struct device *dev)
{
    struct pl061 *p = dev->priv;

    if (!p)
        return;
    mmio_write32(p->base + PL061_IE, 0);
    if (p->irq_intid >= 0 && p->irq_armed)
        irq_disable((unsigned)p->irq_intid);
    kfree(p);
    dev->priv = NULL;
}

struct driver pl061_drv = {
    .name         = "pl061",
    .bus          = &platform_bus,
    .compat_table = (const char *const[]){ "arm,pl061", NULL },
    .probe        = pl_probe,
    .remove       = pl_remove,
};
