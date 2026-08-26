#ifndef I2C_H
#define I2C_H

#include <stdint.h>

/*
 * I2C subsystem (phase 6): adapters register a message-passing
 * transfer op; everything else (addressed reads/writes, presence
 * probes, FDT child discovery) is generic.
 */

#define I2C_M_RD 0x0001                /* read transfer */

struct i2c_msg {
    uint16_t addr;                     /* 7-bit address */
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;
};

struct i2c_adapter {
    const char *name;
    void *priv;
    unsigned speed_khz;

    /*
     * Executes msgs back-to-back as one transaction (repeated
     * starts between messages). Returns 0 or -1.
     */
    int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
                unsigned nmsgs);

    struct i2c_adapter *next;
};

#define I2C_ADAPT_MAX 4

int  i2c_adapter_register(struct i2c_adapter *ad);
struct i2c_adapter *i2c_adapter_find(const char *name);
unsigned i2c_adapter_count(void);

/* convenience wrappers around xfer() */
int i2c_transfer(struct i2c_adapter *ad, struct i2c_msg *msgs,
                 unsigned nmsgs);
int i2c_write_bytes(struct i2c_adapter *ad, uint8_t addr,
                    const uint8_t *src, uint16_t len);
int i2c_read_bytes(struct i2c_adapter *ad, uint8_t addr,
                   uint8_t *dst, uint16_t len);
int i2c_write_reg8(struct i2c_adapter *ad, uint8_t addr,
                   uint8_t reg, uint8_t val);
int i2c_read_reg8(struct i2c_adapter *ad, uint8_t addr,
                  uint8_t reg, uint8_t *val);

/*
 * Presence probe via quick write (no data). Some devices react badly
 * to being probed on live buses; callers should restrict it to setup
 * paths and skip reserved ranges (0x00-0x07, 0x78-0x7f).
 */
int i2c_probe_addr(struct i2c_adapter *ad, uint8_t addr);

/* ---- discovered client records (generic device probing) ------------------------ */

struct i2c_client {
    struct i2c_adapter *adap;
    uint8_t addr;
    const char *compat;                 /* from FDT, or NULL */
    unsigned probed_ok;
};

#define I2C_CLIENT_MAX 16

/*
 * Instantiates client records for every child node of an I2C
 * controller in the device tree ("reg" = 7-bit address,
 * "compatible" carried along). Callers then probe/attach drivers.
 * Returns the number of children found.
 */
int i2c_enumerate_fdt_children(struct i2c_adapter *ad,
                               const void *fdt_blob, int ctrl_node);

#endif /* I2C_H */
