/*
 * battery.c - provider registry, reporting chardev and policy
 * engine (phase 10, item 56 + milestone "battery percentage
 * reported").
 *
 * The QEMU mock discharges 1% every ~6 s of uptime (roughly a
 * one-hour curve) so warnings and the report cadence can be watched
 * live; it refuses to reach the CRITICAL shutdown funnel by policy
 * -- psci_system_off() is reserved for real-gauge data.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "battery.h"
#include "chardev.h"
#include "irq.h"
#include "lib.h"
#include "mm/kheap.h"
#include "platform.h"
#include "pm.h"
#include "psci.h"
#include "spinlock.h"
#include "task.h"
#include "time.h"

#define BATT_SAMPLE_MS      1000u
#define BATT_WARN_PCT       20u
#define BATT_CRIT_PCT        7u
#define BATT_HYST_PCT        2u

static struct {
    spinlock_t lock;

    struct battery_provider *providers;
    unsigned nproviders;
    struct battery_provider *active;

    struct battery_state last;
    uint8_t prev_pct;
    uint64_t last_sample_ms;
    uint32_t warns_issued;

    uint8_t warn_thr, crit_thr;
} batt = {
    .lock       = SPINLOCK_INIT,
    .warn_thr   = BATT_WARN_PCT,
    .crit_thr   = BATT_CRIT_PCT,
};

static const char batt_name[] = "battery";

/* ---- providers ---------------------------------------------------------------- */

int battery_provider_register(struct battery_provider *p)
{
    daif_state s;

    if (!p || !p->name || !p->read)
        return -1;

    spin_lock_irqsave(&batt.lock, &s);
    p->next = batt.providers;
    batt.providers = p;
    batt.nproviders++;
    /* first real (non-mock) registration preempts the mock         */
    if (!batt.active || (!batt.active->is_mock && !p->is_mock) ||
        (batt.active && batt.active->is_mock && !p->is_mock))
        batt.active = p;
    spin_unlock_irqrestore(&batt.lock, s);
    return 0;
}

unsigned battery_provider_count(void)
{
    return batt.nproviders;
}

struct battery_provider *battery_active(void)
{
    return batt.active;
}

bool battery_snapshot_get(struct battery_state *out)
{
    daif_state s;

    spin_lock_irqsave(&batt.lock, &s);
    *out = batt.last;
    spin_unlock_irqrestore(&batt.lock, s);
    return out->present;
}

/* ---- policy (pure) -------------------------------------------------------------- */

enum battery_action battery_policy(uint8_t prev_pct, uint8_t new_pct)
{
    /* hysteresis window so a battery wobbling around a threshold
     * does not flap the log                                        */
    if (new_pct <= batt.crit_thr)
        return BAT_CRITICAL;

    if (prev_pct > batt.warn_thr && new_pct <= batt.warn_thr)
        return BAT_WARN;
    if (prev_pct <= batt.warn_thr + BATT_HYST_PCT &&
        new_pct > batt.warn_thr + BATT_HYST_PCT)
        return BAT_EXIT_WARN;

    return BAT_OK;
}

void battery_thresholds_set(uint8_t warn_pct, uint8_t crit_pct)
{
    daif_state s;

    spin_lock_irqsave(&batt.lock, &s);
    if (warn_pct > 100u) warn_pct = 100u;
    if (crit_pct >= warn_pct) crit_pct = warn_pct ? warn_pct - 1 : 0;
    batt.warn_thr = warn_pct;
    batt.crit_thr = crit_pct;
    spin_unlock_irqrestore(&batt.lock, s);
}

void battery_thresholds_get(uint8_t *w, uint8_t *c)
{
    *w = batt.warn_thr;
    *c = batt.crit_thr;
}


/* ---- QEMU mock provider ------------------------------------------------------------ */

/*
 * 1% per ~6 s of uptime => a visible discharge within a minute and
 * WARN crossing after ~13 minutes of headless runtime, both far
 * inside any CI window. Mock data is marked so the shutdown funnel
 * never mistakes it for reality.
 */
static int mock_read(struct battery_provider *self,
                     struct battery_state *out)
{
    uint64_t sec = time_uptime_ms() / 1000u;

    (void)self;
    out->present     = true;
    out->voltage_mv  = 3300u + (uint16_t)(out->percent * 7u);
    out->current_ma  = -180;            /* light load draw           */
    out->percent     = (uint8_t)(100u - (sec / 6u) % 101u);
    out->temp_deci_c = 310;
    out->age_ms      = (uint32_t)sec;
    return 0;
}

bool battery_mock_attached(void)
{
    struct battery_provider *it;
    daif_state s;

    spin_lock_irqsave(&batt.lock, &s);
    it = batt.active;
    spin_unlock_irqrestore(&batt.lock, s);
    return it && it->is_mock;
}

/* selftest hook: injects a percent directly into the cache         */
void battery_mock_force(uint8_t percent)
{
    struct battery_state st;

    battery_snapshot_get(&st);
    memset(&st, 0, sizeof(st));
    st.present   = true;
    st.percent   = percent;
    st.voltage_mv = 3300;
    st.current_ma = -100;
    st.temp_deci_c = 300;
    st.age_ms    = (uint32_t)time_uptime_ms();

    {
        daif_state s;

        spin_lock_irqsave(&batt.lock, &s);
        batt.last = st;
        spin_unlock_irqrestore(&batt.lock, s);
    }
}


/* ---- housekeeping cadence ---------------------------------------------------------- */

static void report_line(const struct battery_state *st,
                        const char *tag)
{
    kprintf("battery[%s]: %u%% %umV %dmA charger=%s temp=%d.%dC\n",
            tag ? tag : "report",
            st->percent, st->voltage_mv, st->current_ma,
            battery_charger_hint(st->current_ma),
            st->temp_deci_c / 10, (st->temp_deci_c < 0 ? -1 : 1) *
            (st->temp_deci_c % 10));
}

void battery_poll_tick(uint64_t now_ms)
{
    struct battery_state st;
    struct battery_provider *p;
    enum battery_action act;
    daif_state s;
    bool full_report = false;

    if (!batt.active)
        return;

    spin_lock_irqsave(&batt.lock, &s);
    if ((uint32_t)(now_ms - batt.last_sample_ms) < BATT_SAMPLE_MS) {
        spin_unlock_irqrestore(&batt.lock, s);
        return;                         /* 1 Hz sampling ceiling      */
    }
    batt.last_sample_ms = (uint32_t)now_ms;
    spin_unlock_irqrestore(&batt.lock, s);

    p = batt.active;
    memset(&st, 0, sizeof(st));
    if (p->read(p, &st) != 0 || !st.present)
        return;                         /* provider silent this round */

    if (!p->is_mock)
        battery_mock_force(st.percent); /* feed cache uniformly       */
    else
        battery_snapshot_get(&st);      /* cache is the mock truth    */

    spin_lock_irqsave(&batt.lock, &s);
    act = battery_policy(batt.prev_pct, st.percent);

    switch (act) {
    case BAT_WARN:
        batt.warns_issued++;
        break;
    case BAT_CRITICAL:
        batt.warns_issued++;
        break;
    default:
        break;
    }
    batt.prev_pct = st.percent;
    batt.last     = st;
    full_report   = true;
    spin_unlock_irqrestore(&batt.lock, s);

    if (act == BAT_WARN && !p->is_mock) {
        report_line(&st, "LOW");
        kprintf("battery: please connect a charger soon\n");
    }

    /*
     * Shutdown funnel: only REAL gauge data may power the system
     * down -- mock/CI percentages never reach psci_system_off().
     */
    if (act == BAT_CRITICAL && !p->is_mock) {
        report_line(&st, "CRITICAL");
        psci_system_off();              /* orderly shutdown           */
        return;
    } else if (act == BAT_CRITICAL) {
        report_line(&st, "mock-critical-shutdown-suppressed");
    } else if (full_report &&
               (st.percent == 100u ||
                !(time_uptime_ms() % 10000u)))
        report_line(&st, NULL);
}

const char *battery_charger_hint(int current_ma)
{
    if (current_ma > 0)
        return "charging";
    if (current_ma == 0)
        return "idle";
    return "discharging";
}


/* ---- /dev/battery snapshot chardev ----------------------------------------------- */

static int batt_read(struct char_dev *cd, char *dst, unsigned max);

static const struct char_dev batt_chardev = {
    .name  = batt_name,
    .priv  = NULL,
    .read  = batt_read,
    .write = NULL,
    .poll  = NULL,
};

/* minimal decimal emitter for the snapshot line                   */
static void put_dec(char **p, long v)
{
    unsigned long m = v < 0 ? -(unsigned long)v : (unsigned long)v;
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + m % 10u);
        m /= 10u;
    } while (m);
    if (v < 0)
        *(*p)++ = '-';
    while (i)
        *(*p)++ = tmp[--i];
}

static void put_str(char **p, const char *s)
{
    while (*s)
        *(*p)++ = *s++;
}

static int batt_read(struct char_dev *cd, char *dst, unsigned max)
{
    struct battery_state st;
    bool have;
    char buf[96];
    char *w = buf;

    (void)cd;
    have = battery_snapshot_get(&st);

    if (!have || !st.present) {
        put_str(&w, "battery: no gauge\n");
    } else {
        put_str(&w, "battery: ");
        put_dec(&w, st.percent);
        put_str(&w, "% ");
        put_dec(&w, st.voltage_mv);
        put_str(&w, "mV ");
        put_dec(&w, st.current_ma);
        put_str(&w, "mA ");
        put_str(&w, battery_charger_hint(st.current_ma));
        put_str(&w, "\n");
    }

    {
        int n = (int)(w - buf);

        if ((unsigned)n > max)
            n = (int)max;
        memcpy(dst, buf, (size_t)n);
        return n;
    }
}

void battery_subsys_init(const struct platform_info *plat)
{
    (void)plat;

    {
        /* registry wants a mutable pointer (links into its list)   */
        static struct char_dev node;    /* mutable copy               */

        if (!node.name) {
            node      = batt_chardev;
            node.name = batt_name;
        }
        if (char_dev_register(&node))
            kprintf("battery: node registration failed\n");
    }

    /* real PMIC probe ran before us when an adapter existed; with
     * nothing attached by now the QEMU mock covers reporting       */
    if (!batt.active) {
        static struct battery_provider mock = {
            .name    = "mock-qemu",
            .is_mock = true,
            .read    = mock_read,
        };

        if (battery_provider_register(&mock) == 0) {
            kprintf("battery: no gauge found -- attaching "
                    "%s provider\n", mock.name);
        }
    }
}

