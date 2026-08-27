#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

struct platform_info;

/*
 * Battery stack (phase 10, items 55+56 + milestone).
 *
 * Providers report a snapshot; one optional real PMIC driver over
 * I2C ships (pmic_axp.c), and a QEMU-only mock supplies a slowly
 * discharging curve headlessly so percentage reporting / policy
 * transitions are demonstrable without hardware. Policy math is a
 * PURE function shared by runtime + selftest (same pattern as the
 * display-power decision).
 */

struct battery_state {
    bool present;                       /* a live gauge answered      */
    uint16_t voltage_mv;
    int16_t  current_ma;                /* >0 charging                */
    uint8_t  percent;                   /* 0..100                     */
    int16_t  temp_deci_c;
    uint32_t age_ms;                    /* timestamp of this sample   */
};

/* mock-only marker so policy can refuse shutdown on CI data       */
struct battery_provider {
    const char *name;                   /* e.g. "axp209", "mock"      */
    bool is_mock;
    int (*read)(struct battery_provider *self,
                struct battery_state *out);
    void *priv;
    struct battery_provider *next;
};

int      battery_provider_register(struct battery_provider *p);
unsigned battery_provider_count(void);
struct battery_provider *battery_active(void);
bool     battery_snapshot_get(struct battery_state *out);  /* cached */
const char *battery_charger_hint(int current_ma);

/* polling hook driven from housekeeping (~2 ms cadence internally
 * throttled to 1 Hz samples); performs warns/shutdown transitions */
void battery_poll_tick(uint64_t now_ms);

void battery_subsys_init(const struct platform_info *plat);

/* policy knobs (selftest adjusts freely)                          */
void battery_thresholds_set(uint8_t warn_pct, uint8_t crit_pct);
void battery_thresholds_get(uint8_t *warn_pct, uint8_t *crit_pct);

/* pure decision with +-2% hysteresis around thresholds            */
enum battery_action {
    BAT_OK = 0,
    BAT_WARN,                           /* entered low band           */
    BAT_EXIT_WARN,                      /* recovered back above       */
    BAT_CRITICAL,
};

enum battery_action battery_policy(uint8_t prev_pct, uint8_t new_pct);

/* mock controls (debug/test surface, QEMU dev image only)         */
bool battery_mock_attached(void);
void battery_mock_force(uint8_t percent);

#endif /* BATTERY_H */