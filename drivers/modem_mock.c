/*
 * modem_mock.c - scripted QEMU transport (phase 12).
 *
 * Commands written by the AT engine are matched against a prefix
 * table and answered with canned lines after a short simulated
 * latency, delivered through an internal ring that read() drains.
 * The two-stage AT+CMGS flow ("> " prompt, then the PDU terminated
 * by Ctrl-Z) is honoured explicitly. modem_mock_inject_urc() lets
 * the selftest push RING / +CMT lines as if the network sent them.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "modem.h"
#include "time.h"

#define MOCK_RING 512u

static struct {
    uint8_t  rx[MOCK_RING];
    volatile unsigned rx_head, rx_count;

    bool     prompt_pending;        /* waiting for the PDU body    */

    char     sched[6][AT_LINE_MAX];
    uint64_t sched_at[6];
    unsigned nsched;
} mk;

static void mk_push(const char *s)
{
    while (*s && mk.rx_count < MOCK_RING) {
        mk.rx[(mk.rx_head + mk.rx_count) % MOCK_RING] = (uint8_t)*s++;
        mk.rx_count++;
    }
}

static void mk_sched(const char *line, uint32_t delay_ms)
{
    if (mk.nsched >= 6u)
        return;
    memcpy(mk.sched[mk.nsched], line, strlen(line) + 1);
    mk.sched_at[mk.nsched] = time_uptime_ms() + delay_ms;
    mk.nsched++;
}

static int mock_write(void *priv, const void *buf, unsigned len)
{
    const char *s = buf;
    char cmd[AT_CMD_MAX];
    unsigned n = len;

    (void)priv;
    if (n && s[n - 1] == '\r')
        n--;
    if (n >= sizeof(cmd))
        n = sizeof(cmd) - 1u;
    memcpy(cmd, s, n);
    cmd[n] = 0;

    if (mk.prompt_pending) {
        /* PDU body terminated by Ctrl-Z                            */
        mk.prompt_pending = false;
        mk_push("\r\n+CMGS: 1\r\nOK\r\n");
        return (int)len;
    }

    if (!strncmp(cmd, "AT+CMGS", 7)) {
        mk.prompt_pending = true;
        mk_push("> ");
        return (int)len;
    }
    if (!strncmp(cmd, "ATD", 3)) {
        mk_push("\r\nOK\r\n");
        mk_sched("\r\nCONNECT\r\n", 30u);
        return (int)len;
    }
    if (!strncmp(cmd, "ATA", 3)) {
        mk_push("\r\nOK\r\n");
        mk_sched("\r\nCONNECT\r\n", 30u);
        return (int)len;
    }

    /* generic prefix table                                        */
    {
        static const struct {
            const char *prefix;
            const char *resp;
        } table[] = {
            { "AT+CPIN?", "\r\n+CPIN: READY\r\n\r\nOK\r\n" },
            { "AT+CREG?", "\r\n+CREG: 0,1\r\n\r\nOK\r\n" },
            { "AT+CSQ",   "\r\n+CSQ: 18,0\r\n\r\nOK\r\n" },
            { "AT+CGMR",  "\r\nMOCK-MODEM-1.0\r\n\r\nOK\r\n" },
            { "AT+CGPADDR", "\r\n+CGPADDR: 1,\"10.0.2.16\"\r\n\r\nOK\r\n" },
            { "AT",       "\r\nOK\r\n" },
        };

        for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]);
             i++) {
            size_t plen = strlen(table[i].prefix);

            if (!strncmp(cmd, table[i].prefix, plen)) {
                mk_push(table[i].resp);
                return (int)len;
            }
        }
    }

    /* unknown command: vanilla ERROR                              */
    mk_push("\r\nERROR\r\n");
    return (int)len;
}

static int mock_read(void *priv, void *buf, unsigned len)
{
    unsigned n = 0;
    uint64_t now = time_uptime_ms();

    (void)priv;
    /* release due scheduled lines first                            */
    for (unsigned i = 0; i < mk.nsched && n < len; ) {
        if ((int64_t)(now - mk.sched_at[i]) >= 0) {
            const char *src = mk.sched[i];

            while (*src && mk.rx_count < MOCK_RING) {
                mk.rx[(mk.rx_head + mk.rx_count) % MOCK_RING] =
                    (uint8_t)*src++;
                mk.rx_count++;
            }
            /* compact the schedule                                */
            for (unsigned k = i; k + 1u < mk.nsched; k++) {
                memcpy(mk.sched[k], mk.sched[k + 1], AT_LINE_MAX);
                mk.sched_at[k] = mk.sched_at[k + 1];
            }
            mk.nsched--;
            continue;
        }
        i++;
    }

    while (n < len && mk.rx_count) {
        ((uint8_t *)buf)[n] = mk.rx[mk.rx_head];
        mk.rx_head = (mk.rx_head + 1u) % MOCK_RING;
        mk.rx_count--;
        n++;
    }
    return (int)n;
}

bool modem_mock_attached(void)
{
    return true;
}

void modem_mock_inject_urc(const char *line)
{
    char buf[AT_LINE_MAX];
    size_t n = strlen(line);

    /* the engine dispatches on '\n': a caller-supplied line without
     * one (the selftests' bare hex PDU) would sit in the assembly
     * buffer forever, so terminate it here */
    if (n && line[n - 1] == '\n') {
        mk_sched(line, 0u);
        return;
    }
    if (n > sizeof(buf) - 3u)
        n = sizeof(buf) - 3u;
    memcpy(buf, line, n);
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    mk_sched(buf, 0u);
}

/* transport constructor exported for the modem layer              */
bool modem_mock_transport(struct at_transport *out)
{
    out->write = mock_write;
    out->read  = mock_read;
    out->priv  = NULL;
    return true;
}
