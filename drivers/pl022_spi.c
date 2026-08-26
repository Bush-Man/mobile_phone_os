/*
 * pl022_spi.c - ARM PrimeCell PL022 SPI controller backend.
 *
 * Master mode, polling, 8-bit frames. Chip select is left to gpiolib
 * lines (see spi_core); the PL022 itself only shifts data.
 *
 * Not present on QEMU `-M virt`; instantiated by board bring-up via
 * pl022_register().
 */

#include <stdint.h>
#include <stddef.h>

#include "lib.h"
#include "mm/kheap.h"
#include "mm/types.h"
#include "mmio.h"
#include "spi.h"
#include "spinlock.h"

#define SSP_CR0     0x00    /* data size, FRF, SPO, SPH, SCR */
#define SSP_CR1     0x04    /* SSE, MS, SOD */
#define SSP_DR      0x08
#define SSP_SR      0x0c
#define SSP_CPSR    0x10    /* CPSDVSR: 2..254, even */
#define SSP_ICR     0x48

#define SR_TFE   (1u << 0)
#define SR_TNF   (1u << 1)
#define SR_RNE   (1u << 2)
#define SR_BSY   (1u << 4)

#define CR1_SSE  (1u << 1)

#define SPIN_LIMIT 100000u

struct pl022 {
    uintptr_t base;
    unsigned input_hz;                  /* SSPCLK */
    spinlock_t lock;

    struct spi_controller ctlr;         /* priv back-pointer */
};

static struct pl022 *s_pl022[2];
static unsigned s_npl022;

static void pl_disable(uintptr_t b)
{
    mmio_write32(b + SSP_CR1,
                 mmio_read32(b + SSP_CR1) & ~CR1_SSE);
}

static int pl_configure(struct spi_controller *ctlr, unsigned max_hz,
                        uint8_t mode)
{
    struct pl022 *p = ctlr->priv;
    uintptr_t b = p->base;
    unsigned cpsr, scr;

    if (!p->input_hz)
        p->input_hz = 100000000;        /* typical SSPCLK guess */

    /*
     * bit rate = SSPCLK / (CPSDVR * (1 + SCR)); pick the largest even
     * prescale <= needed, then the smallest SCR that fits.
     */
    {
        unsigned want = p->input_hz / (max_hz ? max_hz : 400000);
        unsigned best_cpsr = 254, best_scr = 255;
        unsigned best_err = ~0u;

        for (cpsr = 2; cpsr <= 254; cpsr += 2) {
            for (scr = 0; scr <= 255; scr++) {
                unsigned div = cpsr * (scr + 1);
                int err = (int)div - (int)want;

                if (err < 0)
                    err = -err;
                if ((unsigned)err < best_err) {
                    best_err = (unsigned)err;
                    best_cpsr = cpsr;
                    best_scr = scr;
                    if (!best_err)
                        goto found;
                }
            }
        }
found:
        cpsr = best_cpsr;
        scr = best_scr;
    }

    pl_disable(b);

    /* 8-bit frames, motorola SPI, clock polarity/phase from mode */
    uint32_t cr0 = 7u |                     /* DSS = 8 bits          */
                  (((uint32_t)(mode & SPI_MODE_CPOL)) << 6) |
                  (((uint32_t)(mode & SPI_MODE_CPHA)) << 7) |
                  ((uint32_t)scr << 8);

    mmio_write32(b + SSP_CR0, cr0);
    mmio_write32(b + SSP_CPSR, cpsr);
    mmio_write32(b + SSP_CR1, CR1_SSE);     /* master, enabled */
    return 0;
}

static int pl_transfer(struct spi_controller *ctlr,
                       const uint8_t *tx, uint8_t *rx, unsigned len)
{
    struct pl022 *p = ctlr->priv;
    uintptr_t b = p->base;
    daif_state s;
    unsigned t;

    spin_lock_irqsave(&p->lock, &s);

    for (unsigned i = 0; i < len; i++) {
        for (t = 0; t < SPIN_LIMIT && !(mmio_read32(b + SSP_SR) & SR_TNF); t++)
            ;
        if (t == SPIN_LIMIT)
            goto timeout;

        mmio_write32(b + SSP_DR, tx ? tx[i] : 0xffu);

        for (t = 0; t < SPIN_LIMIT && !(mmio_read32(b + SSP_SR) & SR_RNE); t++)
            ;
        if (t == SPIN_LIMIT)
            goto timeout;

        uint8_t got = (uint8_t)(mmio_read32(b + SSP_DR) & 0xffu);

        if (rx)
            rx[i] = got;
    }

    /* drain until the shift register empties before CS release */
    for (t = 0; t < SPIN_LIMIT && (mmio_read32(b + SSP_SR) & SR_BSY); t++)
        ;
    if (t == SPIN_LIMIT)
        goto timeout;

    spin_unlock_irqrestore(&p->lock, s);
    return 0;

timeout:
    spin_unlock_irqrestore(&p->lock, s);
    return -1;
}

int pl022_register(uintptr_t base, unsigned input_clock_hz,
                   const char *name)
{
    struct pl022 *p;

    if (s_npl022 >= ARRAY_SIZE(s_pl022))
        return -1;

    p = kzalloc(sizeof(*p));
    if (!p)
        return -1;

    p->base = base;
    p->input_hz = input_clock_hz;
    p->lock = (spinlock_t)SPINLOCK_INIT;
    p->ctlr.name = name;
    p->ctlr.priv = p;
    p->ctlr.configure = pl_configure;
    p->ctlr.transfer = pl_transfer;

    pl_disable(base);                   /* idle until configured */
    mmio_write32(base + SSP_ICR, 3);    /* clear stale irqs */

    if (spi_register_controller(&p->ctlr) != 0) {
        kfree(p);
        return -1;
    }

    s_pl022[s_npl022++] = p;
    return 0;
}
