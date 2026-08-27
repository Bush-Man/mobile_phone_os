/*
 * pmic_axp.c - PMIC over I2C, X-Powers AXP-family scaffold (phase
 * 10, plan item 55).
 *
 * Probes the phase-6 i2c registry for classic AXP addresses; when a
 * chip ACKs, a battery_provider is registered whose read() gathers
 * voltage / current / temperature / charger status per the documented
 * AXP209-style register map (constants below carry their unit
 * scalings so board bring-up tunes data, not structure). Fuel-gauge
 * percent is approximated from pack voltage by the LiPo LUT marked
 * EXPERIMENTAL -- real coulomb counting is deferred to phase-16 HW
 * bring-up.
 *
 * QEMU -M virt instantiates no I2C controller: adapter_count()==0,
 * this driver logs one line and never registers. The battery stack
 * then attaches its mock provider automatically.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "battery.h"
#include "i2c.h"
#include "lib.h"
#include "platform.h"

/* ---- register map ---------------------------------------------------------------- */

#define AXP_REG_CHIP_ID     0x03u   /* family id byte             */
#define AXP_REG_BATV_HI     0x78u   /* BAT voltage msb            */
#define AXP_REG_BATV_LO     0x79u   /* lsb; 12-bit @ 1.25 mV/LSB  */
#define AXP_REG_DISCHG_HI   0x7cu   /* discharge current          */
#define AXP_REG_CHGST       0x01u   /* bit6 = charging flag       */
#define AXP_REG_TEMP_HI     0x5eu   /* internal temp, deci-C      */

static const uint8_t axp_addrs[] = { 0x34u, 0x36u };

struct pmic_softc {
    struct battery_provider prov;
    struct i2c_adapter *adap;
    uint8_t addr;
};

static struct pmic_softc sc;

/* ---- helpers ------------------------------------------------------------------------ */

static uint16_t rd12(struct i2c_adapter *ad, uint8_t addr,
                     uint8_t hi_reg)
{
    uint8_t hi = 0, lo = 0;

    if (i2c_read_reg8(ad, addr, hi_reg, &hi) ||
        i2c_read_reg8(ad, addr, (uint8_t)(hi_reg + 1), &lo))
        return 0xffffu;
    return (uint16_t)(((uint32_t)hi << 4) | (lo >> 4));
}

/* EXPERIMENTAL open-circuit-voltage approximation for LiPo packs */
static uint8_t volt_to_pct(uint16_t mv)
{
    static const uint16_t lut_mv[] = {
        3300u, 3480u, 3610u, 3690u, 3750u, 3800u, 3870u,
        3940u, 4010u, 4090u, 4200u,
    };
    const unsigned n = sizeof(lut_mv) / sizeof(lut_mv[0]);
    unsigned pct = 0;

    for (unsigned i = 0; i < n; i++)
        if (mv >= lut_mv[i])
            pct = i * 100u / (n - 1u);
    return (uint8_t)pct;
}

/* ---- provider ------------------------------------------------------------------------- */

static int axp_read(struct battery_provider *prov,
                    struct battery_state *out)
{
    struct pmic_softc *d =
        (struct pmic_softc *)((char *)prov -
                              offsetof(struct pmic_softc, prov));
    struct i2c_adapter *ad = d->adap;
    uint8_t addr = d->addr;
    uint16_t mv_raw, ma_raw;
    uint8_t chg = 0, tmp_hi = 0, tmp_lo = 0;

    memset(out, 0, sizeof(*out));

    mv_raw = rd12(ad, addr, AXP_REG_BATV_HI);
    if (mv_raw == 0xffffu)
        return -1;                       /* bus vanished             */
    out->voltage_mv = (uint16_t)((uint32_t)mv_raw * 1250u / 1000u);

    ma_raw = rd12(ad, addr, AXP_REG_DISCHG_HI);
    out->current_ma = (ma_raw == 0xffffu) ?
                          0 : -(int16_t)(ma_raw / 2u); /* .5 mA LSB*/

    if (!i2c_read_reg8(ad, addr, AXP_REG_CHGST, &chg)) {
        if (chg & 0x40u)                 /* charging bit set         */
            out->current_ma = -out->current_ma;
    }

    if (!i2c_read_reg8(ad, addr, AXP_REG_TEMP_HI, &tmp_hi) &&
        !i2c_read_reg8(ad, addr, (uint8_t)(AXP_REG_TEMP_HI + 1),
                       &tmp_lo)) {
        int32_t raw = ((int32_t)tmp_hi << 8) | tmp_lo;

        /* AXP209 formula ends at -144.7 C offset; approximate   */
        out->temp_deci_c = (int16_t)(raw / 5u - 1447);
    }

    out->present = true;
    out->percent = volt_to_pct(out->voltage_mv);
    return 0;
}

/* ---- probe ------------------------------------------------------------------------------ */

void pmic_axp_probe(void)
{
    unsigned nadapts = i2c_adapter_count();

    for (unsigned a = 0; a < nadapts && !sc.adap; a++) {
        struct i2c_adapter *ad = i2c_adapter_at(a);

        if (!ad)
            continue;
        for (unsigned k = 0; k < sizeof(axp_addrs); k++) {
            uint8_t addr = axp_addrs[k];
            uint8_t chipid = 0;

            if (i2c_probe_addr(ad, addr))
                continue;
            if (i2c_read_reg8(ad, addr, AXP_REG_CHIP_ID,
                              &chipid))
                continue;

            sc.adap = ad;
            sc.addr = addr;
            sc.prov.name    = "axp-pmic";
            sc.prov.is_mock = false;
            sc.prov.read    = axp_read;
            sc.prov.priv    = NULL;
            break;
        }
    }

    if (!sc.adap || !sc.prov.read) {
        kprintf("pmic-axp: no PMIC on any I2C adapter\n");
        return;
    }

    if (battery_provider_register(&sc.prov)) {
        kprintf("pmic-axp: registration refused\n");
        memset(&sc, 0, sizeof(sc));
        return;
    }
    kprintf("pmic-axp: gauge online at %s 0x%02x\n",
            sc.adap->name ? sc.adap->name : "i2c", sc.addr);
}
