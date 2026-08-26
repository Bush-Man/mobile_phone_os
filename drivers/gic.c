/*
 * gic.c - GICv2 interrupt controller backend (distributor + CPU
 * interface), as emulated by QEMU virt and present on the Pi 4's
 * GIC-400. GICv3 differs enough (system-register CPU interface,
 * re-distributor discovery, group/enable routing) that it gets its
 * own backend later; gic_init() detects the version from the FDT
 * probe and refuses to continue on anything but v2 so the failure is
 * loud instead of silent.
 */

#include <stdint.h>

#include "gic.h"
#include "irq.h"
#include "mmio.h"
#include "panic.h"
#include "platform.h"

/* ---- distributor register map ------------------------------------------ */
#define GICD_CTLR       0x000u
#define GICD_TYPER      0x004u
#define GICD_IIDR       0x008u
#define GICD_IGROUPR    0x080u      /* +4*n, n = intid/32            */
#define GICD_ISENABLER  0x100u
#define GICD_ICENABLER  0x180u
#define GICD_ISPENDR    0x200u
#define GICD_ICPENDR    0x280u
#define GICD_IPRIORITYR 0x400u      /* byte per intid                */
#define GICD_ITARGETSR  0x800u      /* byte per intid, SGIs/PPIs RO  */
#define GICD_ICFGR      0xc00u      /* 2 bits per intid              */
#define GICD_SGIR       0xf00u

#define GICD_CTLR_ENABLE_GRP0   (1u << 0)
#define GICD_CTLR_ENABLE_GRP1   (1u << 1)

/* SGIR TargetListFilter field */
#define SGIR_FILTER_SELF        (2u << 24)

/* ---- cpu interface register map ----------------------------------------- */
#define GICC_CTLR       0x000u
#define GICC_PMR        0x004u
#define GICC_IAR        0x00cu
#define GICC_EOIR       0x010u

#define GICC_CTLR_ENABLE_GRP0   (1u << 0)
#define GICC_CTLR_ENABLE_GRP1   (1u << 1)

/* ICFGR encodes two bits per intid: b10 = edge triggered */
#define ICFGR_EDGE_PAIR         0x2u

static uintptr_t gicd;
static uintptr_t gicc;
static uint32_t self_mask;      /* this cpu's GIC interface mask     */

static uint32_t prio_word(unsigned prio)
{
    return 0x01010101u * (prio & 0xffu);
}

static unsigned nr_enable_words(void)
{
    /* GICD_TYPER.ITLinesNumber = number of ISENABLER words - 1 */
    return ((mmio_read32(gicd + GICD_TYPER) & 0x1fu) + 1u);
}

/* word RMW on a byte-per-intid register (IPRIORITYR / ITARGETSR) */
static void set_byte_reg(unsigned base, unsigned intid, uint32_t val,
                         unsigned shift)
{
    uintptr_t addr = gicd + base + 4u * (intid / 4u);
    uint32_t w = mmio_read32(addr);

    w &= ~(0xffu << shift);
    w |= val << shift;
    mmio_write32(addr, w);
}

void gic_irq_enable(unsigned intid)
{
    mmio_write32(gicd + GICD_ISENABLER + 4u * (intid / 32u),
                 1u << (intid % 32u));
}

void gic_irq_disable(unsigned intid)
{
    mmio_write32(gicd + GICD_ICENABLER + 4u * (intid / 32u),
                 1u << (intid % 32u));
}

void gic_irq_set_priority(unsigned intid, unsigned prio)
{
    set_byte_reg(GICD_IPRIORITYR, intid, prio & 0xffu,
                 8u * (intid % 4u));
}

void gic_irq_set_trigger_edge(unsigned intid, bool edge)
{
    uintptr_t addr;
    unsigned pair = (intid % 16u) * 2u;
    uint32_t w;

    if (intid < IRQ_SPI_BASE)
        return;                         /* SGI config is read-only */
    addr = gicd + GICD_ICFGR + 4u * (intid / 16u);
    w = mmio_read32(addr) & ~(3u << pair);
    if (edge)
        w |= ICFGR_EDGE_PAIR << pair;
    mmio_write32(addr, w);
}

void gic_send_sgi(unsigned sgi)
{
    /* TargetListFilter=0b10 forwards only to this interface */
    mmio_write32(gicd + GICD_SGIR, SGIR_FILTER_SELF | (sgi & 0xfu));
}

void gic_send_sgi_list(uint8_t iface_mask, unsigned sgi)
{
    /* TargetListFilter=0b00 forwards to the listed interfaces */
    mmio_write32(gicd + GICD_SGIR,
                 ((uint32_t)(iface_mask & 0xffu) << 16) | (sgi & 0xfu));
}

uint8_t gic_self_iface_mask(void)
{
    return (uint8_t)self_mask;
}

uint32_t gic_ack(void)
{
    return mmio_read32(gicc + GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
    /*
     * EOImode=0: one write performs priority drop AND deactivation,
     * which is all we need while interrupts never nest.
     */
    mmio_write32(gicc + GICC_EOIR, iar);
}

static void dist_init(void)
{
    unsigned words = nr_enable_words();
    unsigned i;

    /* our own interface mask, read back from the banked SGI region */
    self_mask = mmio_read32(gicd + GICD_ITARGETSR) & 0xffu;

    mmio_write32(gicd + GICD_CTLR, 0);          /* quiesce first       */

    for (i = 0; i < words; i++) {               /* disable + unpending */
        mmio_write32(gicd + GICD_ICENABLER + 4u * i, 0xffffffffu);
        mmio_write32(gicd + GICD_ICPENDR + 4u * i, 0xffffffffu);
    }

    /* route every SPI to this interface; SGI/PPI region ignores it */
    for (i = 1; i < words; i++)
        mmio_write32(gicd + GICD_ITARGETSR + 4u * i,
                     self_mask | self_mask << 8 |
                     self_mask << 16 | self_mask << 24);

    /* uniform mid-table default priority; drivers raise theirs */
    for (i = 0; i < words; i++)
        mmio_write32(gicd + GICD_IPRIORITYR + 4u * i, prio_word(0xa0));

    /* level-sensitive defaults across PPI+SPI; SGIs are fixed-edge */
    for (i = 1; i < words * 2u; i++)            /* ICFGR word = 16 ids */
        mmio_write32(gicd + GICD_ICFGR + 4u * i, 0);

    /*
     * Keep everything Group 0. Without security extensions (QEMU
     * virt with a direct -kernel boot) a Group 1 interrupt is never
     * signalled to EL1 -- the interface just reports 1022 "pending
     * but not visible", so marking lines Group 1 here would silence
     * the entire controller. Group 1 routing gets revisited only if
     * we ever run behind secure firmware that needs it.
     */
    for (i = 0; i < words; i++)
        mmio_write32(gicd + GICD_IGROUPR + 4u * i, 0);

    mmio_write32(gicd + GICD_CTLR, GICD_CTLR_ENABLE_GRP0);
}

static void cpu_init(void)
{
    /* pass every priority through the running threshold */
    mmio_write32(gicc + GICC_PMR, 0xffu);
    mmio_write32(gicc + GICC_CTLR, GICC_CTLR_ENABLE_GRP0);
}

/*
 * Per-cpu (banked) bring-up: distributor init happens exactly once on
 * the boot cpu; every secondary calls only this.
 */
void gic_cpu_init(void)
{
    cpu_init();
}

void gic_init(const struct platform_info *plat)
{
    if (!plat->has_gic)
        panic("gic: no interrupt controller found in device tree");
    if (plat->gic_version != 2)
        panic("gic: GICv3 backend not brought up yet");

    gicd = plat->gicd_base;
    gicc = plat->gicc_base;

    dist_init();
    cpu_init();
}
