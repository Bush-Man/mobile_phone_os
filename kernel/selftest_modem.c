/*
 * selftest_modem.c - phase 12 verification, run as the "modtest"
 * task against the scripted mock modem (deterministic headlessly;
 * the same code paths run against a real UART modem on boards).
 *
 * Checks, in order:
 *   1. SIM ready + registration (home) + signal parse (rssi 18).
 *   2. Outbound call: dial -> OK -> CONNECT urc -> ACTIVE; hangup
 *      -> NO CARRIER -> IDLE; audio routing hook saw the changes.
 *   3. Inbound call: RING urc -> INCOMING; answer -> ACTIVE.
 *   4. SMS send: two-stage CMGS with a real SUBMIT PDU built by the
 *      7-bit encoder (mock responds +CMGS: 1).
 *   5. SMS receive: mock injects +CMT header + hex DELIVER PDU;
 *      decoder recovers sender/text, /sms store round-trips via VFS.
 *   6. Data: rmnet0 netif up/down through the phase-11 registry.
 *
 * Summary "selftest: modem ok" matches the harness style.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "lib.h"
#include "modem.h"
#include "net.h"
#include "task.h"
#include "time.h"

static int failures;

#define CHECK(cond, name)                                              \
    do {                                                               \
        if (cond) {                                                    \
            kprintf("modtest: %-34s ok\n", name);                      \
        } else {                                                       \
            kprintf("modtest: %-34s FAIL\n", name);                    \
            failures++;                                                \
        }                                                              \
    } while (0)

/* ---- call observation ------------------------------------------------------------ */

static struct {
    unsigned routes;
    enum call_state last;
    bool saw_active, saw_idle_after;
} obs;

static void route_observer(enum call_event e, void *arg)
{
    (void)arg;
    obs.routes++;
    obs.last = call_ctl_state();
    if (call_ctl_state() == CALL_ACTIVE)
        obs.saw_active = true;
    if (call_ctl_state() == CALL_IDLE)
        obs.saw_idle_after = true;
    kprintf("modtest: call event %d -> %s\n", (int)e,
            call_state_name(call_ctl_state()));
}

/* ---- sms receive observation --------------------------------------------------------- */

static struct {
    char     sender[SMS_ADDR_MAX];
    char     text[SMS_TEXT_MAX];
    volatile bool got;
} rxobs;

static void sms_rx_observer(const char *sender, const char *text,
                            void *arg)
{
    (void)arg;
    memcpy(rxobs.sender, sender, strlen(sender) + 1u);
    memcpy(rxobs.text, text, strlen(text) + 1u);
    rxobs.got = true;
}

/* ---- 1: status ------------------------------------------------------------------- */

static void status_tests(void)
{
    bool sim = false;
    enum reg_status reg = REG_UNKNOWN;
    struct modem_signal sig = { 99, 99 };

    CHECK(modem_query_sim_ready(&sim, 3000u) == 0 && sim,
          "sim ready");
    CHECK(modem_query_reg(&reg, 3000u) == 0 && reg == REG_HOME,
          "registered home");
    CHECK(modem_query_signal(&sig, 3000u) == 0 &&
              sig.rssi == 18u && sig.ber == 0u,
          "signal rssi 18 ber 0");
}

/* ---- 2: outbound call --------------------------------------------------------------- */

static void dial_tests(void)
{
    obs.routes = 0;
    obs.saw_active = obs.saw_idle_after = false;

    CHECK(modem_dial("5550001") == 0, "dial submitted");
    msleep(80);                         /* let OK + CONNECT land      */
    modem_tick(time_uptime_ms());
    CHECK(obs.saw_active, "outbound call ACTIVE");

    CHECK(modem_hangup() == 0, "hangup submitted");
    msleep(60);
    modem_tick(time_uptime_ms());
    CHECK(obs.saw_idle_after, "hangup -> IDLE");
    CHECK(obs.routes >= 3u, "audio route hook fired");
}

/* ---- 3: inbound call -------------------------------------------------------------------- */

static void incoming_tests(void)
{
    obs.saw_active = false;

    modem_mock_inject_urc("\r\nRING\r\n");
    modem_tick(time_uptime_ms());
    CHECK(call_ctl_state() == CALL_INCOMING, "RING -> INCOMING");

    CHECK(modem_answer() == 0, "answer submitted");
    msleep(80);
    modem_tick(time_uptime_ms());
    CHECK(obs.saw_active, "inbound call ACTIVE");

    modem_mock_inject_urc("\r\nNO CARRIER\r\n");
    modem_tick(time_uptime_ms());
    CHECK(call_ctl_state() == CALL_IDLE, "remote hangup -> IDLE");
}

/* ---- 4+5: sms ---------------------------------------------------------------------------- */

static void sms_tests(void)
{
    /* send: two-stage CMGS with the 7-bit SUBMIT PDU               */
    CHECK(modem_sms_send("5551234", "PHASE12 SMS OK") == 0,
          "sms send (cmgs + submit pdu)");

    /* receive: inject a DELIVER PDU as hex after a +CMT header     */
    {
        uint8_t pdu[64];
        char hex[140];
        unsigned o = 0;
        int plen;

        /* handcrafted DELIVER: flags 04, OA "5551234" (7 digits),
         * PID 00, DCS 00, SCTS 7 zero bytes, UDL 15, "INBOX TEST 1" */
        pdu[0] = 0x04;
        pdu[1] = 0x07;
        pdu[2] = 0x81;
        pdu[3] = 0x51; pdu[4] = 0x15; pdu[5] = 0x32;
        pdu[6] = 0xF1;
        pdu[7] = 0x00;                  /* PID                        */
        pdu[8] = 0x00;                  /* DCS                        */
        for (int i = 0; i < 7; i++)
            pdu[9 + i] = 0x00;          /* SCTS                       */
        pdu[16] = 0x0Fu;                /* UDL: 15 septets            */
        {
            static const char txt[] = "INBOX TEST 1";
            int nb = sms_encode_7bit(txt, &pdu[17],
                                     sizeof(pdu) - 17u);

            if (nb < 0)
                return;
        }
        plen = 17 + sms_encode_7bit("INBOX TEST 1",
                                    &pdu[17], sizeof(pdu) - 17u);

        for (int i = 0; i < plen; i++) {
            static const char hd[] = "0123456789ABCDEF";

            hex[o++] = hd[pdu[i] >> 4];
            hex[o++] = hd[pdu[i] & 0x0fu];
        }
        hex[o] = 0;

        rxobs.got = false;
        modem_mock_inject_urc("\r\n+CMT: ,24\r\n");
        modem_mock_inject_urc(hex);
        modem_tick(time_uptime_ms());
        modem_tick(time_uptime_ms());

        CHECK(rxobs.got && !strcmp(rxobs.sender, "5551234") &&
                  !strcmp(rxobs.text, "INBOX TEST 1"),
              "sms decoded sender+text");
    }

    /* store round-trip through the VFS                            */
    {
        char sender[SMS_ADDR_MAX], text[SMS_TEXT_MAX];
        char name[24];
        unsigned last = sms_seq();

        {
            char num[8];
            unsigned v = last, i = 0, o = 0;

            memcpy(name, "/sms/msg", 8);
            do {
                num[i++] = (char)('0' + v % 10u);
                v /= 10u;
            } while (v);
            o = 8;
            while (i)
                name[o++] = num[--i];
            name[o] = 0;
        }

        CHECK(sms_read_msg(name, sender, sizeof(sender),
                           text, sizeof(text)) == 0 &&
                  !strcmp(sender, "5551234") &&
                  !strcmp(text, "INBOX TEST 1"),
              "sms vfs store round-trip");
    }
}

/* ---- 6: data --------------------------------------------------------------------------- */

static void data_tests(void)
{
    struct netif nif;
    unsigned before = netif_count();

    CHECK(modem_data_up(&nif) == 0, "data session up");
    CHECK(netif_count() == before + 1u, "rmnet0 in registry");
    CHECK(modem_data_down() == 0, "data session down");
}

/* ---- entry ------------------------------------------------------------------------------ */

void modem_selftest_task(void *arg)
{
    (void)arg;

    kprintf("modtest: phase 12 modem selftests\n");

    modem_set_call_handler(route_observer, NULL);
    modem_set_sms_handler(sms_rx_observer, NULL);

    status_tests();
    dial_tests();
    incoming_tests();
    sms_tests();
    data_tests();

    if (!failures)
        kprintf("selftest: modem ok\n");
    else
        kprintf("selftest: modem FAILED (%d)\n", failures);

    task_exit();
}
