/*
 * modemd.c - telephony service broker (phase 15).
 *
 * The phase-12 modem layer is kernel-resident (AT engine, call
 * state machine, SMS store); EL0 apps need a service path. This
 * kernel task publishes a line-based protocol over the phase-8
 * unix transport at "/var/run/modem":
 *
 *   PING              -> PONG
 *   STATUS            -> STATE <call-state-name>
 *   DIAL <number>     -> OK / ERR <errno>
 *   ANSWER            -> OK / ERR <errno>
 *   HANGUP            -> OK / ERR <errno>
 *   SMS <num> <text>  -> OK / ERR <errno>
 *   REG               -> REG <n>
 *   SIGNAL            -> SIG <rssi> <ber>
 *
 * and fans modem events out to every connected client as
 *   EV CALL <event-name>
 *   EV SMS <sender> <text>
 *
 * Each accepted connection gets its own reader task (blocking
 * line reads), so the dialer, the messaging app and the phase-15
 * selftest can be connected simultaneously -- the "dialer/SMS over
 * a cellular modem" seam the plan describes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib.h"
#include "modem.h"
#include "syscall.h"
#include "task.h"
#include "unsock.h"
#include "vfs.h"

#define MD_LINE_MAX   96
#define MD_CLIENTS     8

/* lib.h has no strchr; local helper keeps the unit self-contained */
static char *md_strchr(const char *s, char c)
{
    for (;; s++) {
        if (*s == c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

struct md_client {
    struct file *conn;              /* NULL = free slot            */
};

static struct file *md_listener;
static struct md_client md_clients[MD_CLIENTS];
static spinlock_t md_lock = SPINLOCK_INIT;

/* ---- line builder (no kernel snprintf; crash.c's pattern) ------- */

struct lb {
    char     *b;
    unsigned  cap, pos;
};

static void lb_str(struct lb *l, const char *s)
{
    while (*s && l->pos + 1 < l->cap)
        l->b[l->pos++] = *s++;
}

static void lb_i(struct lb *l, long v)
{
    char tmp[20];
    int n = 0;
    unsigned long m = v < 0 ? (unsigned long)(-(v + 1)) + 1u
                            : (unsigned long)v;

    if (v < 0 && l->pos + 1 < l->cap)
        l->b[l->pos++] = '-';
    do {
        tmp[n++] = (char)('0' + m % 10u);
        m /= 10u;
    } while (m && n < (int)sizeof(tmp));
    while (n && l->pos + 1 < l->cap)
        l->b[l->pos++] = tmp[--n];
}

/* line ends: terminate + return length                             */
static unsigned lb_end(struct lb *l)
{
    if (l->pos + 1 < l->cap)
        l->b[l->pos++] = '\n';
    l->b[l->pos] = 0;
    return l->pos;
}

/* ---- event fan-out -------------------------------------------------- */

static const char *md_call_ev_name(enum call_event e)
{
    switch (e) {
    case CALL_EV_DIAL:          return "DIAL";
    case CALL_EV_OK:            return "OK";
    case CALL_EV_CONNECT:       return "CONNECT";
    case CALL_EV_RING:          return "RING";
    case CALL_EV_INCOMING:      return "INCOMING";
    case CALL_EV_ANSWER:        return "ANSWER";
    case CALL_EV_HANGUP_LOCAL:  return "HANGUP-LOCAL";
    case CALL_EV_HANGUP_REMOTE: return "HANGUP-REMOTE";
    case CALL_EV_BUSY:          return "BUSY";
    case CALL_EV_ERROR:         return "ERROR";
    default:                    return "?";
    }
}

static void md_event_line(const char *line, unsigned len)
{
    daif_state s;

    spin_lock_irqsave(&md_lock, &s);
    for (int i = 0; i < MD_CLIENTS; i++) {
        if (md_clients[i].conn) {
            /* blocking write is fine: handler context is the
             * housekeeping task; events are short and the
             * unix buffers drain                             */
            f_write(md_clients[i].conn, line, len);
        }
    }
    spin_unlock_irqrestore(&md_lock, s);
}

static void md_call_event(enum call_event e, void *arg)
{
    char line[48];
    struct lb l = { line, sizeof(line), 0 };

    (void)arg;
    lb_str(&l, "EV CALL ");
    lb_str(&l, md_call_ev_name(e));
    md_event_line(line, lb_end(&l));
}

static void md_sms_rx(const char *sender, const char *text, void *arg)
{
    char line[224];
    struct lb l = { line, sizeof(line), 0 };

    (void)arg;
    lb_str(&l, "EV SMS ");
    lb_str(&l, sender ? sender : "?");
    lb_str(&l, " ");
    lb_str(&l, text ? text : "");
    md_event_line(line, lb_end(&l));
}

/* ---- line dispatch ---------------------------------------------------- */

static bool md_line_is(const char *line, const char *cmd)
{
    size_t n = strlen(cmd);

    return strncmp(line, cmd, n) == 0 &&
           (line[n] == 0 || line[n] == ' ');
}

static void md_ok_err(struct file *conn, int r)
{
    char buf[24];
    struct lb l = { buf, sizeof(buf), 0 };

    if (r == 0)
        lb_str(&l, "OK");
    else {
        lb_str(&l, "ERR ");
        lb_i(&l, r);
    }
    f_write(conn, buf, lb_end(&l));
}

static void md_dispatch(struct file *conn, char *line)
{
    char buf[64];
    struct lb l = { buf, sizeof(buf), 0 };

    if (md_line_is(line, "PING")) {
        lb_str(&l, "PONG");
        f_write(conn, buf, lb_end(&l));
    } else if (md_line_is(line, "STATUS")) {
        lb_str(&l, "STATE ");
        lb_str(&l, call_state_name(call_ctl_state()));
        f_write(conn, buf, lb_end(&l));
    } else if (md_line_is(line, "DIAL ")) {
        md_ok_err(conn, modem_dial(line + 5));
    } else if (md_line_is(line, "ANSWER")) {
        md_ok_err(conn, modem_answer());
    } else if (md_line_is(line, "HANGUP")) {
        md_ok_err(conn, modem_hangup());
    } else if (md_line_is(line, "SMS ")) {
        char *sp = md_strchr(line + 4, ' ');
        int r;

        if (sp) {
            *sp = 0;
            r = modem_sms_send(line + 4, sp + 1);
        } else {
            r = -EINVAL;
        }
        md_ok_err(conn, r);
    } else if (md_line_is(line, "REG")) {
        enum reg_status reg = REG_UNKNOWN;

        modem_query_reg(&reg, 2000);
        lb_str(&l, "REG ");
        lb_i(&l, (long)reg);
        f_write(conn, buf, lb_end(&l));
    } else if (md_line_is(line, "SIGNAL")) {
        struct modem_signal sig = { 99, 0 };

        modem_query_signal(&sig, 2000);
        lb_str(&l, "SIG ");
        lb_i(&l, (long)sig.rssi);
        lb_str(&l, " ");
        lb_i(&l, (long)sig.ber);
        f_write(conn, buf, lb_end(&l));
    } else {
        lb_str(&l, "ERR ");
        lb_i(&l, -EINVAL);
        f_write(conn, buf, lb_end(&l));
    }
}

static void md_client_task(void *arg)
{
    struct file *conn = arg;
    char buf[64];
    char line[MD_LINE_MAX];
    unsigned line_len = 0;

    for (;;) {
        long r = f_read(conn, buf, sizeof(buf));

        if (r <= 0)
            break;                  /* peer closed / error         */

        for (long i = 0; i < r; i++) {
            char c = buf[i];

            if (c == '\n' || c == '\r') {
                if (line_len) {
                    line[line_len] = 0;
                    md_dispatch(conn, line);
                    line_len = 0;
                }
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }

    {
        daif_state s;

        spin_lock_irqsave(&md_lock, &s);
        for (int i = 0; i < MD_CLIENTS; i++)
            if (md_clients[i].conn == conn)
                md_clients[i].conn = NULL;
        spin_unlock_irqrestore(&md_lock, s);
    }
    file_close(conn);
    task_exit();
}

void modemd_task(void *arg)
{
    (void)arg;

    vfs_mkdir("/var");
    vfs_mkdir("/var/run");

    if (usock_serve("/var/run/modem", &md_listener) != 0) {
        kprintf("modemd: serve /var/run/modem failed\n");
        task_exit();
    }

    modem_set_call_handler(md_call_event, NULL);
    modem_set_sms_handler(md_sms_rx, NULL);

    kprintf("modemd: serving /var/run/modem\n");

    for (;;) {
        struct file *conn;
        daif_state s;
        int slot = -1;

        if (usock_accept(md_listener, &conn) < 0)
            continue;

        spin_lock_irqsave(&md_lock, &s);
        for (int i = 0; i < MD_CLIENTS; i++)
            if (!md_clients[i].conn) {
                slot = i;
                break;
            }
        if (slot >= 0)
            md_clients[slot].conn = conn;
        spin_unlock_irqrestore(&md_lock, s);

        if (slot < 0) {
            file_close(conn);       /* table full                  */
            continue;
        }

        {
            int tid = task_create("modemcli", md_client_task,
                                  conn, 40);

            if (tid < 0) {
                spin_lock_irqsave(&md_lock, &s);
                md_clients[slot].conn = NULL;
                spin_unlock_irqrestore(&md_lock, s);
                file_close(conn);
            }
        }
    }
}
