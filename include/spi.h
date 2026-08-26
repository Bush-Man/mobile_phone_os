#ifndef SPI_H
#define SPI_H

#include <stdint.h>

/*
 * SPI subsystem (phase 6): controllers register a full-duplex
 * transfer op plus speed/mode programming; devices carry chip-select
 * GPIO lines (driven by the gpiolib) since PL022-class controllers
 * have no native CS handling.
 */

#define SPI_MODE_CPHA 0x01
#define SPI_MODE_CPOL 0x02

struct spi_controller {
    const char *name;
    void *priv;

    int (*configure)(struct spi_controller *, unsigned max_hz,
                     uint8_t mode);
    /* full-duplex shift of len bytes; rx may be NULL */
    int (*transfer)(struct spi_controller *,
                    const uint8_t *tx, uint8_t *rx, unsigned len);

    struct spi_controller *next;
};

struct spi_device {
    struct spi_controller *ctlr;
    unsigned max_speed_hz;
    uint8_t mode;                       /* CPOL/CPHA bits */
    int cs_gpio;                        /* gpiolib line, -1 if none */

    const char *compat;
    void *priv;
};

#define SPI_CTRL_MAX 4

int  spi_register_controller(struct spi_controller *ctlr);
unsigned spi_ctrl_count(void);

/* configure + drive CS + transfer + release CS */
int  spi_sync(struct spi_device *dev,
              const uint8_t *tx, uint8_t *rx, unsigned len);
int  spi_write_then_read(struct spi_device *dev,
                         const uint8_t *wr, unsigned wr_len,
                         uint8_t *rd, unsigned rd_len);

#endif /* SPI_H */
