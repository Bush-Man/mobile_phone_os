/*
 * selftest_ui.c - the phase-15 battery ("uitest15").
 *
 * Runs as a kernel task after init has spawned the compositor
 * daemon; every check drives the real phone UX end to end:
 *
 *   1. protocol  -- spawn the "uitest" EL0 client and reap exit
 *                   code 0 (HELLO/OPEN/shm map/SHOW/FOCUS/NOTIFY
 *                   all answered; window composited).
 *   2. sms       -- inject a DELIVER PDU into the mock modem;
 *                   modemd decodes + fans out "EV SMS" and the
 *                   compositor raises a notification banner
 *                   ("[ui] banner: SMS ..." on serial, checked by
 *                   the harness). Kernel-side we assert the /sms
 *                   store grew.
 *   3. unlock    -- push synthetic touches through input_push()
 *                   at the include/ui_layout.h numpad centers:
 *                   PIN 1234 + OK must flip the compositor to the
 *                   home screen ("[ui] unlock ok" on serial).
 *   4. launch    -- tap the Dialer launcher icon; the compositor
 *                   forks/execs the dialer, which opens its own
 *                   window ("[ui] launch dialer" + "[dialer]
 *                   ready").
 *   5. ring      -- inject a RING URC; the dialer's modem
 *                   connection and the compositor both see the
 *                   "EV CALL RING" fan-out (call banner).
 *
 * The compositor's serial lines are the milestone proof; kernel
 * CHECKs cover everything observable from EL1.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "input.h"
#include "lib.h"
#include "modem.h"
#include "proc.h"
#include "task.h"
#include "time.h"
#include "ui_layout.h"

static int fails;

#define CHECK(cond, name)                                     \
    do {                                                      \
        if (cond) {                                           \
            kprintf("uitest15: %-38s ok\n", name);            \
        } else {                                              \
            kprintf("uitest15: %-38s FAIL\n", name);          \
            fails++;                                          \
        }                                                     \
    } while (0)

/* ---- synthetic touch ------------------------------------------------- */

/* one tap == the /dev/event0 record shape: down, move, report,
 * up, report (the compositor acts on the down+report)          */
static void tap_at(unsigned x, unsigned y)
{
    input_push(EV_KEY, BTN_TOUCH, 1);
    input_push(EV_ABS, ABS_X, (int32_t)x);
    input_push(EV_ABS, ABS_Y, (int32_t)y);
    input_push(EV_SYN, SYN_REPORT, 0);
    input_push(EV_KEY, BTN_TOUCH, 0);
    input_push(EV_SYN, SYN_REPORT, 0);
}

static void tap_key(unsigned row, unsigned col)
{
    tap_at(UI_KEY_CX(col), UI_KEY_CY(row));
    msleep(250);
}

/* ---- steps ------------------------------------------------------------ */

static void wait_for_compositor(void)
{
    unsigned waited;

    for (waited = 0; waited < 200u; waited++) {
        if (proc_pid_of_name("compositor") >= 0)
            break;
        msleep(100);
    }
    CHECK(proc_pid_of_name("compositor") >= 0,
          "compositor process up");
    msleep(1500);                   /* socket + threads settle  */
}

static void test_protocol(void)
{
    int pid, code, rc;

    pid = proc_spawn("uitest",
                     (const char *const[]){ "uitest", NULL },
                     NULL);
    CHECK(pid > 0, "uitest spawn");
    if (pid <= 0)
        return;

    rc = proc_do_waitpid(pid, &code);
    CHECK(rc == pid, "uitest reaped");
    CHECK(code == 0, "ui protocol round trip");
}

/* DELIVER PDU with sender "5551234" / text "PHASE15 SMS", the
 * same handcrafted layout the phase-12 battery used            */
static void inject_sms(void)
{
    static const char txt[] = "PHASE15 SMS";
    uint8_t pdu[64];
    char hex[140];
    unsigned o = 0, plen;
    unsigned seq_before = sms_seq();

    if (!modem_mock_attached()) {
        kprintf("uitest15: %-38s skip (no mock)\n", "sms inject");
        return;
    }

    pdu[0] = 0x04;                  /* flags: DELIVER           */
    pdu[1] = 0x07;                  /* OA len (digits)          */
    pdu[2] = 0x81;                  /* TOaN/ToN                 */
    pdu[3] = 0x51;                  /* "5551234" swizzled       */
    pdu[4] = 0x15;
    pdu[5] = 0x32;
    pdu[6] = 0xF1;
    pdu[7] = 0x00;                  /* PID                      */
    pdu[8] = 0x00;                  /* DCS                      */
    for (int i = 0; i < 7; i++)
        pdu[9 + i] = 0x00;          /* SCTS                     */
    pdu[16] = 0x0Fu;                /* UDL: 15 septets          */
    plen = 17u + (unsigned)sms_encode_7bit(txt, &pdu[17],
                                           sizeof(pdu) - 17u);

    for (unsigned i = 0; i < plen; i++) {
        static const char hd[] = "0123456789ABCDEF";

        hex[o++] = hd[pdu[i] >> 4];
        hex[o++] = hd[pdu[i] & 0x0fu];
    }
    hex[o] = 0;

    modem_mock_inject_urc("\r\n+CMT: ,24\r\n");
    modem_mock_inject_urc(hex);
    modem_tick(time_uptime_ms());
    modem_tick(time_uptime_ms());
    msleep(1500);                   /* fan-out + banner render  */

    CHECK(sms_seq() > seq_before, "sms decoded (banner fanned)");
}

static void test_unlock(void)
{
    /* PIN 1234: keys 1,2,3 (row 0), 4 (row 1 col 0), then OK
     * (row 3, col 2) -- centers from include/ui_layout.h       */
    tap_key(0, 0);
    tap_key(0, 1);
    tap_key(0, 2);
    tap_key(1, 0);
    tap_key(3, 2);
    msleep(500);                    /* compositor prints proof  */

    tap_at(UI_ICON_CX(0), UI_ICON_CY(0));   /* Dialer icon      */
    msleep(2000);                   /* fork/exec + app connect  */
}

static void test_ring(void)
{
    if (!modem_mock_attached()) {
        kprintf("uitest15: %-38s skip (no mock)\n", "ring inject");
        return;
    }
    modem_mock_inject_urc("\r\nRING\r\n");
    modem_tick(time_uptime_ms());
    modem_tick(time_uptime_ms());
    msleep(1000);                   /* dialer + banner react    */
}

void ui_selftest_task(void *arg)
{
    (void)arg;

    msleep(500);                    /* let init spawn settle    */
    wait_for_compositor();

    test_protocol();
    inject_sms();
    test_unlock();
    test_ring();

    if (fails == 0)
        kprintf("selftest: ui ok\n");
    else
        kprintf("selftest: ui FAILURES (%d)\n", fails);
    task_exit();
}
