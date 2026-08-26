/*
 * i2c_dw.c - Synopsys DesignWare I2C controller backend.
 *
 * Common IP across ARM SoCs (PMICs and touch controllers usually
 * hang off one). Polling, 7-bit addressing, standard/fast speeds,
 * write-then-read transactions with a repeated start.
 *
 * Not present on QEMU `-M virt`; instantiated by board bring-up via
 * dw_i2c_register(). Kept dormant until real hardware phases.
 */

#include <stdint.h>
#include <stddef.h>

#include "i2c.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/types.h"
#include "mmio.h"
#include "spinlock.h"

/* ---- registers ---------------------------------------------------------------- */

#define DW_CON          0x00
#define DW_TAR          0x04
#define DW_DATA_CMD     0x10    /* byte | CMD_READ(8) | STOP(9) */
#define DW_SS_SCL_HCNT  0x14
#define DW_SS_SCL_LCNT  0x18
#define DW_FS_SCL_HCNT  0x1c
#define DW_FS_SCL_LCNT  0x20
#define DW_INTR_MASK    0x30
#define DW_RX_TL        0x38
#define DW_TX_TL        0x3c
#define DW_CLR_INTR     0x40
#define DW_CLR_TX_ABRT  0x54
#define DW_CLR_STOP_DET 0x60
#define DW_ENABLE       0x6c
#define DW_STATUS       0x70
#define DW_TXFLR        0x74
#define DW_RXFLR        0x78
#define DW_TX_ABRT_SRC  0x80
#define DW_ENABLE_STATUS 0x9c

#define CON_MASTER_EN      (1u << 0)
#define CON_SPEED_STD      (1u << 1)
#define CON_SPEED_FAST     (2u << 1)
#define CON_RESTART_EN     (1u << 5)
#define CON_SLAVE_DISABLE  (1u << 6)

#define STAT_TFNF       (1u << 1)       /* tx fifo not full */
#define STAT_RFNE       (1u << 3)       /* rx fifo not empty */

#define DC_CMD_READ     (1u << 8)
#define DC_STOP         (1u << 9)

#define SPIN_LIMIT 1000000u

struct dw_i2c {
    uintptr_t base;
    unsigned clock_hz;                  /* input bus clock */
    spinlock_t lock;

    struct i2c_adapter adap;            /* back-pointer via priv */
};

static struct dw_i2c *s_dw[2];
static unsigned s_ndw;

/* ---- low level ------------------------------------------------------------------ */

static void dw_disable(uintptr_t b)
{
    mmio_write32(b + DW_ENABLE, 0);
    for (unsigned t = 0; t < SPIN_LIMIT; t++)
        if (!(mmio_read32(b + DW_ENABLE_STATUS) & 1u))
            return;
}

static int dw_set_speed(struct dw_i2c *d, unsigned khz)
{
    uintptr_t b = d->base;
    uint32_t con = CON_MASTER_EN | CON_RESTART_EN | CON_SLAVE_DISABLE;
    uint32_t hcnt_reg = DW_SS_SCL_HCNT, lcnt_reg = DW_SS_SCL_LCNT;

    if (!d->clock_hz)
        d->clock_hz = 24000000;         /* typical APB input */

    if (khz > 100) {
        con |= CON_SPEED_FAST;
        hcnt_reg = DW_FS_SCL_HCNT;
        lcnt_reg = DW_FS_SCL_LCNT;
    } else {
        con |= CON_SPEED_STD;
    }

    /*
     * Counts are clock cycles of the high/low half-period; the IP's
     * minimums (fast: high 6, low 13; std: high 28, low 47) floor us.
     */
    {
        unsigned period = d->clock_hz / (khz ? khz : 100);
        unsigned hcnt = period / 3;
        unsigned lcnt = period - hcnt;

        if (khz > 100) {
            if (hcnt < 6)
                hcnt = 6;
            if (lcnt < 13)
                lcnt = 13;
        } else {
            if (hcnt < 28)
                hcnt = 28;
            if (lcnt < 47)
                lcnt = 47;
        }
        mmio_write32(b + hcnt_reg, hcnt);
        mmio_write32(b + lcnt_reg, lcnt);
    }

    mmio_write32(b + DW_CON, con);
    return 0;
}

/* ---- transfer engine -------------------------------------------------------------- */

static int dw_xfer(struct i2c_adapter *ad, struct i2c_msg *msgs,
                   unsigned nmsgs)
{
    struct dw_i2c *d = ad->priv;
    uintptr_t b = d->base;
    daif_state s;
    int r = 0;

    if (nmsgs > 2)
        return -1;

    spin_lock_irqsave(&d->lock, &s);

    dw_disable(b);
    mmio_write32(b + DW_INTR_MASK, 0);          /* polling mode */
    (void)mmio_read32(b + DW_CLR_INTR);
    (void)mmio_read32(b + DW_CLR_TX_ABRT);
    (void)mmio_read32(b + DW_CLR_STOP_DET);

    mmio_write32(b + DW_TAR, msgs[0].addr & 0x7fu);
    mmio_write32(b + DW_RX_TL, 0);
    mmio_write32(b + DW_TX_TL, 4);

    mmio_write32(b + DW_ENABLE, 1);

    for (unsigned m = 0; m < nmsgs && r == 0; m++) {
        struct i2c_msg *msg = &msgs[m];
        bool rd = !!(msg->flags & I2C_M_RD);
        bool last = (m == nmsgs - 1);
        unsigned got = 0;

        for (unsigned i = 0; i < msg->len; i++) {
            uint32_t cmd;
            unsigned t;

            if (rd)
                cmd = DC_CMD_READ;
            else {
                cmd = msg->buf[i];
                for (t = 0; t < SPIN_LIMIT &&
                            !(mmio_read32(b + DW_STATUS) & STAT_TFNF); t++)
                    ;
                if (t == SPIN_LIMIT) {
                    r = -1;
                    break;
                }
            }

            if (last && i + 1 == msg->len)
                cmd |= DC_STOP;

            mmio_write32(b + DW_DATA_CMD, cmd);

            if (rd) {
                for (t = 0; t < SPIN_LIMIT &&
                            !(mmio_read32(b + DW_RXFLR)); t++)
                    ;
                if (t == SPIN_LIMIT) {
                    r = -1;
                    break;
                }
                msg->buf[got++] =
                    (uint8_t)(mmio_read32(b + DW_DATA_CMD) & 0xffu);
            }
        }

        if (mmio_read32(b + DW_TX_ABRT_SRC)) {
            (void)mmio_read32(b + DW_CLR_TX_ABRT);      /* NACK etc. */
            r = -1;
        }
    }

    (void)mmio_read32(b + DW_CLR_STOP_DET);
    dw_disable(b);
    (void)mmio_read32(b + DW_CLR_INTR);

    spin_unlock_irqrestore(&d->lock, s);
    return r;
}

/* ---- registration -------------------------------------------------------------------- */

int dw_i2c_register(uintptr_t base, unsigned input_clock_hz,
                    const char *name)
{
    struct dw_i2c *d;

    if (s_ndw >= ARRAY_SIZE(s_dw))
        return -1;

    d = kzalloc(sizeof(*d));
    if (!d)
        return -1;

    d->base = base;
    d->clock_hz = input_clock_hz;
    d->lock = (spinlock_t)SPINLOCK_INIT;
    d->adap.name = name;
    d->adap.priv = d;
    d->adap.speed_khz = 400;
    d->adap.xfer = dw_xfer;

    dw_disable(base);
    dw_set_speed(d, 400);

    if (i2c_adapter_register(&d->adap) != 0) {
        kfree(d);
        return -1;
    }

    s_dw[s_ndw++] = d;
    return 0;
}
