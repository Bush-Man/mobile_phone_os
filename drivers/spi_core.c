/*
 * spi_core.c - controller registry and CS-managed transactions.
 */

#include <stdint.h>
#include <stddef.h>

#include "gpio.h"
#include "lib.h"
#include "spi.h"

static struct spi_controller *ctrls[SPI_CTRL_MAX];
static unsigned nctrls;

int spi_register_controller(struct spi_controller *ctlr)
{
    if (!ctlr || !ctlr->name || !ctlr->configure || !ctlr->transfer)
        return -1;
    for (unsigned i = 0; i < nctrls; i++)
        if (ctrls[i] == ctlr)
            return 0;                   /* idempotent */
    if (nctrls >= SPI_CTRL_MAX)
        return -1;
    ctrls[nctrls++] = ctlr;
    kprintf("spi: controller %s\n", ctlr->name);
    return 0;
}

unsigned spi_ctrl_count(void)
{
    return nctrls;
}

static void cs_assert(struct spi_device *dev, bool on)
{
    if (dev->cs_gpio == -1)
        return;
    gpio_dir_out((unsigned)dev->cs_gpio, !on);  /* CS is active low */
}

int spi_sync(struct spi_device *dev,
             const uint8_t *tx, uint8_t *rx, unsigned len)
{
    if (!dev || !dev->ctlr || len == 0)
        return -1;
    if (dev->ctlr->configure(dev->ctlr, dev->max_speed_hz, dev->mode) != 0)
        return -1;

    int r;

    cs_assert(dev, true);
    r = dev->ctlr->transfer(dev->ctlr, tx, rx, len);
    cs_assert(dev, false);
    return r;
}

/*
 * Classic sensor transaction: write command/pointer bytes, then read
 * the answer with a fresh chip-select frame.
 */
int spi_write_then_read(struct spi_device *dev,
                        const uint8_t *wr, unsigned wr_len,
                        uint8_t *rd, unsigned rd_len)
{
    if (!dev || !wr || !wr_len || !rd || !rd_len)
        return -1;
    if (dev->ctlr->configure(dev->ctlr, dev->max_speed_hz, dev->mode) != 0)
        return -1;

    int r1, r2;

    if (rd_len > 64)
        return -1;                      /* single-frame reads only */

    cs_assert(dev, true);
    r1 = dev->ctlr->transfer(dev->ctlr, wr, NULL, wr_len);

    uint8_t zeros[64];
    for (unsigned i = 0; i < rd_len; i++)
        zeros[i] = 0;

    r2 = dev->ctlr->transfer(dev->ctlr, zeros, rd, rd_len);
    cs_assert(dev, false);

    return (r1 != 0) ? r1 : r2;
}
