/*
 * modem.c - the modem layer (phase 12): ties the AT engine to the
 * call state machine, the SMS store and the status parsers.
 *
 * URC classification: RING / CONNECT / NO CARRIER / +CMT drive the
 * call state machine and the SMS receive path; everything else goes
 * to the registered generic URC handler. The two-stage CMGS flow
 * (prompt, then PDU + Ctrl-Z) is sequenced with a small state
 * machine fed by response callbacks.
 *
 * modem_tick() (housekeeping cadence) drains the engine. Submits
 * return immediately; the query helpers wait on a done flag with
 * msleep polls (they only ever run in task contexts).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "lib.h"
#include "modem.h"
#include "net.h"
#include "platform.h"
#include "time.h"

static struct at_engine eng;
static bool inited;

static modem_call_event_fn call_fn;
static void *call_arg;
static modem_sms_rx_fn sms_fn;
static void *sms_arg;

/* ---- query plumbing ------------------------------------------------------------ */

static volatile bool q_done;
static volatile int  q_status;
static const char   *q_lines[AT_RESP_MAX];
static unsigned      q_nlines;

static void q_collect(int status, const char *const *lines,
                      unsigned nlines, void *arg)
{
    (void)arg;
    q_status = status;
    q_nlines = nlines < AT_RESP_MAX ? nlines : AT_RESP_MAX;
    for (unsigned i = 0; i < q_nlines; i++)
        q_lines[i] = lines[i];
    q_done = true;
}

static int q_wait(uint32_t timeout_ms)
{
    uint64_t deadline = time_uptime_ms() + timeout_ms;

    while (!q_done) {
        if ((long)(time_uptime_ms() - deadline) >= 0)
            return -1;
        modem_tick(time_uptime_ms());
        msleep(2);
    }
    return 0;
}

/* ---- URC + response classification ------------------------------------------------ */

static void urc_line(const char *line, void *arg)
{
    (void)arg;

    if (!strncmp(line, "RING", 4)) {
        call_ctl_apply(CALL_EV_INCOMING);
        call_ctl_apply(CALL_EV_RING);
        if (call_fn)
            call_fn(CALL_EV_INCOMING, call_arg);
        return;
    }
    if (!strncmp(line, "CONNECT", 7)) {
        call_ctl_apply(CALL_EV_CONNECT);
        if (call_fn)
            call_fn(CALL_EV_CONNECT, call_arg);
        return;
    }
    if (!strncmp(line, "NO CARRIER", 10)) {
        call_ctl_apply(CALL_EV_HANGUP_REMOTE);
        if (call_fn)
            call_fn(CALL_EV_HANGUP_REMOTE, call_arg);
        return;
    }
    if (!strncmp(line, "BUSY", 4)) {
        call_ctl_apply(CALL_EV_BUSY);
        if (call_fn)
            call_fn(CALL_EV_BUSY, call_arg);
        return;
    }
    if (!strncmp(line, "+CMT:", 5)) {
        /*
         * +CMT: [<alpha>,]<length>
         * the PDU hex follows on the next line; the engine delivers
         * it as the following URC -- the sms sink pairs them.
         */
        extern void modem_sms_sink_line(const char *line);

        modem_sms_sink_line(line);
        return;
    }
    kprintf("modem: urc %s\n", line);
}

/* ---- init ------------------------------------------------------------------------- */

void modem_subsys_init(const struct platform_info *plat)
{
    struct at_transport tp;
    bool have = false;

    (void)plat;
    if (inited)
        return;

    extern bool modem_uart_transport(struct at_transport *out);
    extern bool modem_mock_transport(struct at_transport *out);

    if (modem_uart_transport(&tp)) {
        have = true;
        kprintf("modem: uart transport\n");
    } else if (modem_mock_transport(&tp)) {
        have = true;
        kprintf("modem: mock transport (QEMU)\n");
    }
    if (!have)
        return;

    at_engine_init(&eng, &tp);
    at_engine_set_urc_handler(&eng, urc_line, NULL);
    call_ctl_init();
    inited = true;
}

bool modem_present(void)
{
    return inited;
}

void modem_tick(uint64_t now_ms)
{
    if (inited)
        at_engine_tick(&eng, now_ms);
}

void modem_set_call_handler(modem_call_event_fn fn, void *arg)
{
    call_fn  = fn;
    call_arg = arg;
}

void modem_set_sms_handler(modem_sms_rx_fn fn, void *arg)
{
    sms_fn  = fn;
    sms_arg = arg;
}

/* ---- dial / answer / hangup ---------------------------------------------------------- */

int modem_dial(const char *number)
{
    char cmd[AT_CMD_MAX];

    if (!inited || !number || call_ctl_state() != CALL_IDLE)
        return -1;

    call_ctl_apply(CALL_EV_DIAL);
    {
        const char *pre = "ATD";
        size_t i = 0;

        while (*pre && i < sizeof(cmd) - 1u)
            cmd[i++] = *pre++;
        while (*number && i < sizeof(cmd) - 1u)
            cmd[i++] = *number++;
        cmd[i] = 0;
    }
    if (at_engine_submit(&eng, cmd, 5000u, 1u, q_collect, NULL))
        return -1;
    return q_wait(6000u);
}

int modem_answer(void)
{
    if (!inited || call_ctl_state() != CALL_INCOMING)
        return -1;
    if (at_engine_submit(&eng, "ATA", 3000u, 1u, q_collect, NULL))
        return -1;
    return q_wait(4000u);
}

int modem_hangup(void)
{
    if (!inited)
        return -1;
    call_ctl_apply(CALL_EV_HANGUP_LOCAL);
    if (at_engine_submit(&eng, "ATH", 2000u, 1u, q_collect, NULL))
        return -1;
    return q_wait(3000u);
}

static int q_submit_wait(const char *cmd, uint32_t timeout_ms)
{
    if (at_engine_submit(&eng, cmd, timeout_ms, 1u, q_collect, NULL))
        return -1;
    return q_wait(timeout_ms + 1000u);
}

static bool line_prefix(const char *line, const char *prefix)
{
    while (*line == ' ')
        line++;
    return !strncmp(line, prefix, strlen(prefix));
}

static long atoi_mod(const char *s)
{
    long v = 0;
    bool neg = false;

    if (*s == '-') {
        neg = true;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10u + (*s++ - '0');
    return neg ? -v : v;
}

int modem_query_sim_ready(bool *ready, uint32_t timeout_ms)
{
    if (!inited || !ready)
        return -1;
    if (q_submit_wait("AT+CPIN?", timeout_ms))
        return -1;
    *ready = (q_nlines >= 1u &&
              line_prefix(q_lines[0], "+CPIN: READY"));
    return 0;
}

int modem_query_reg(enum reg_status *out, uint32_t timeout_ms)
{
    if (!inited || !out)
        return -1;
    if (q_submit_wait("AT+CREG?", timeout_ms))
        return -1;
    *out = REG_UNKNOWN;
    for (unsigned i = 0; i < q_nlines; i++) {
        if (line_prefix(q_lines[i], "+CREG:")) {
            const char *comma = strchr(q_lines[i], ',');
            long stat = 99;

            if (comma)
                stat = atoi_mod(comma + 1);
            *out = (stat >= 0 && stat <= 5)
                       ? (enum reg_status)stat : REG_UNKNOWN;
            return 0;
        }
    }
    return -1;
}

int modem_query_signal(struct modem_signal *out, uint32_t timeout_ms)
{
    if (!inited || !out)
        return -1;
    if (q_submit_wait("AT+CSQ", timeout_ms))
        return -1;
    out->rssi = 99u;
    out->ber  = 99u;
    for (unsigned i = 0; i < q_nlines; i++) {
        if (line_prefix(q_lines[i], "+CSQ:")) {
            const char *comma = strchr(q_lines[i], ',');

            out->rssi = (uint8_t)atoi_mod(q_lines[i] + 5);
            if (comma)
                out->ber = (uint8_t)atoi_mod(comma + 1);
            return 0;
        }
    }
    return -1;
}


/* ---- sms (item 67) ------------------------------------------------------------ */

static struct {
    bool     sending;               /* two-stage CMGS in progress  */
    char     to[SMS_ADDR_MAX];
    char     text[SMS_TEXT_MAX];
    volatile bool sent_ok;
    volatile int  mr;
} sms_tx;

static void cmgs_response(int status, const char *const *lines,
                          unsigned nlines, void *arg)
{
    (void)arg;
    sms_tx.sent_ok = (status == AT_OK);
    for (unsigned i = 0; i < nlines && !sms_tx.mr; i++)
        if (line_prefix(lines[i], "+CMGS:"))
            sms_tx.mr = (int)atoi_mod(lines[i] + 6);
}

static void cmgs_prompt(int status, const char *const *lines,
                        unsigned nlines, void *arg)
{
    uint8_t pdu[180];
    int plen;
    static char body[2 * 180 + 4];
    unsigned o = 0;

    (void)status;
    (void)lines;
    (void)nlines;
    (void)arg;

    plen = sms_build_submit_pdu(sms_tx.to, sms_tx.text, pdu,
                                sizeof(pdu));
    if (plen < 0) {
        sms_tx.sent_ok = false;
        q_done = true;
        q_status = AT_ERROR;
        return;
    }

    for (int i = 0; i < plen; i++) {
        static const char hexdig[] = "0123456789ABCDEF";

        body[o++] = hexdig[pdu[i] >> 4];
        body[o++] = hexdig[pdu[i] & 0x0fu];
    }
    body[o++] = 0x1a;               /* Ctrl-Z terminator           */
    body[o] = 0;

    if (at_engine_submit(&eng, body, 5000u, 0u,
                         cmgs_response, NULL)) {
        sms_tx.sent_ok = false;
        q_done = true;
        q_status = AT_ERROR;
    }
}

int modem_sms_send(const char *to, const char *text)
{
    char cmd[32];
    size_t tl = strlen(text);

    if (!inited || !to || !text)
        return -1;
    if (tl > SMS_TEXT_MAX)
        return -1;

    {
        uint8_t tmp[160];

        if (sms_encode_7bit(text, tmp, sizeof(tmp)) < 0)
            return -1;
    }

    memset(&sms_tx, 0, sizeof(sms_tx));
    {
        size_t n = strlen(to);

        if (n >= SMS_ADDR_MAX)
            n = SMS_ADDR_MAX - 1u;
        memcpy(sms_tx.to, to, n);
    }
    memcpy(sms_tx.text, text, tl);

    {
        char *w = cmd;
        const char *pre = "AT+CMGS=";
        char num[8];
        unsigned v = (unsigned)tl, i = 0, o;

        while (*pre)
            *w++ = *pre++;
        do {
            num[i++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        o = i;
        while (o)
            *w++ = num[--o];
        *w = 0;
    }

    sms_tx.sent_ok = false;
    if (at_engine_submit(&eng, cmd, 3000u, 0u,
                         cmgs_prompt, NULL))
        return -1;

    /* wait for the two-stage flow to finish                       */
    {
        uint64_t deadline = time_uptime_ms() + 12000u;

        while (eng.pending) {
            if ((long)(time_uptime_ms() - deadline) >= 0)
                return -1;
            msleep(2);
        }
    }
    return sms_tx.sent_ok ? 0 : -1;
}


/* ---- receive path: +CMT pairing (line = header, next = PDU hex) */

static struct {
    bool     await_pdu;
    char     hexbuf[340];
    unsigned hexlen;
} cmt;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void cmt_finish(void)
{
    uint8_t pdu[170];
    unsigned plen = cmt.hexlen / 2u;
    char sender[SMS_ADDR_MAX], text[SMS_TEXT_MAX];

    for (unsigned i = 0; i + 1u < cmt.hexlen + 1u && i / 2u < sizeof(pdu);
         i += 2) {
        int hi = hexval(cmt.hexbuf[i]);
        int lo = (i + 1u < cmt.hexlen) ? hexval(cmt.hexbuf[i + 1u]) : 0;

        if (hi < 0 || lo < 0)
            break;
        pdu[i / 2u] = (uint8_t)((hi << 4) | lo);
    }

    if (sms_parse_deliver_pdu(pdu, plen, sender, sizeof(sender),
                              text, sizeof(text)) == 0) {
        sms_store_inbox(sender, text);
        if (sms_fn)
            sms_fn(sender, text, sms_arg);
    }
    cmt.await_pdu = false;
}

void modem_sms_sink_line(const char *line)
{
    if (!cmt.await_pdu) {
        cmt.await_pdu = true;
        cmt.hexlen = 0;
        return;
    }

    {
        const char *p = line;

        while (*p && cmt.hexlen + 2u < sizeof(cmt.hexbuf)) {
            if (hexval(*p) >= 0)
                cmt.hexbuf[cmt.hexlen++] = *p;
            p++;
        }
    }
    cmt_finish();
}

/* ---- data connection (item 68) ------------------------------------------------ */

static struct netif rmnet;
static bool rmnet_up;

static int rmnet_out(struct netif *nif, const uint8_t *dest_hw,
                     uint16_t ethertype, const void *buf, unsigned len)
{
    /*
     * Real data plane = PPP over the serial transport (or a
     * modem-native rmnet interface) -- HW bring-up documented in
     * docs/PHASE_12.md. Until then the interface exists in the
     * registry so phase-11 routing/table paths stay live, and
     * outbound frames are dropped here.
     */
    (void)nif; (void)dest_hw; (void)ethertype; (void)buf; (void)len;
    return -1;
}

int modem_data_up(struct netif *nif_out)
{
    if (!inited || rmnet_up)
        return -1;

    memset(&rmnet, 0, sizeof(rmnet));
    rmnet.name = "rmnet0";
    rmnet.ip_addr  = IP4_SLIRP_GUEST;
    rmnet.netmask  = 0xffffff00u;
    rmnet.gw       = IP4_SLIRP_GW;
    rmnet.mtu      = ETH_MTU;
    rmnet.up       = true;
    rmnet.link_out = rmnet_out;

    if (netif_register(&rmnet))
        return -1;
    rmnet_up = true;
    if (nif_out)
        *nif_out = rmnet;
    kprintf("modem: data session up (rmnet0 registered)\n");
    return 0;
}

int modem_data_down(void)
{
    rmnet_up = false;
    return 0;
}

