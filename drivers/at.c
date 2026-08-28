/*
 * at.c - AT command engine (phase 12, plan item 64).
 *
 * Line assembly from the transport byte stream, URC classification,
 * response collection (up to AT_RESP_MAX intermediate lines before
 * the final OK/ERROR), timeout enforcement with the caller-specified
 * retry count. Never sleeps: at_engine_tick() runs from housekeeping
 * and both transport ops may sleep safely because their callers are
 * task contexts (housekeeping itself, or blocking submits via the
 * modem layer's wait-for-done loops).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "modem.h"
#include "time.h"

static void deliver(struct at_engine *e, int status)
{
    at_resp_fn cb = e->resp_cb;
    void *arg = e->resp_arg;

    e->pending  = false;
    e->resp_cb  = NULL;
    e->resp_arg = NULL;

    if (cb)
        cb(status, (const char *const *)e->resp_lines,
           e->resp_count, arg);
}

static void deliver_error(struct at_engine *e, int status)
{
    if (status == AT_TIMEOUT)
        e->stats.timeouts++;
    else
        e->stats.errors++;
    deliver(e, status);
}

void at_engine_init(struct at_engine *e, struct at_transport *tp)
{
    memset(e, 0, sizeof(*e));
    e->tp = *tp;
}

void at_engine_set_urc_handler(struct at_engine *e, at_urc_fn fn,
                               void *arg)
{
    e->urc_cb  = fn;
    e->urc_arg = arg;
}

int at_engine_submit(struct at_engine *e, const char *cmd,
                     uint32_t timeout_ms, unsigned retries,
                     at_resp_fn cb, void *arg)
{
    char line[AT_CMD_MAX + 4u];
    unsigned clen;

    if (e->pending || !cmd)
        return -1;

    clen = (unsigned)strlen(cmd);
    if (clen > AT_CMD_MAX)
        return -1;

    memcpy(line, cmd, clen);
    line[clen++] = '\r';

    if (e->tp.write(e->tp.priv, line, clen) < 0)
        return -1;

    memcpy(e->retry_cmd, cmd, clen);   /* includes NUL               */
    e->pending     = true;
    e->resp_cb     = cb;
    e->resp_arg    = arg;
    e->resp_count  = 0;
    e->sent_ms     = time_uptime_ms();
    e->timeout_ms  = timeout_ms ? timeout_ms : 2000u;
    e->retries_left = retries;
    e->stats.submitted++;
    return 0;
}

/* collect one response line; returns 1 when final (OK/ERROR)      */
static int line_consume(struct at_engine *e, const char *line)
{
    if (!strcmp(line, "OK")) {
        e->stats.ok++;
        deliver(e, AT_OK);
        return 1;
    }
    if (!strncmp(line, "ERROR", 5) ||
        !strncmp(line, "+CME ERROR", 10) ||
        !strncmp(line, "+CMS ERROR", 10)) {
        deliver_error(e, AT_ERROR);
        return 1;
    }

    if (!e->pending) {
        /* unsolicited: everything while idle is a URC             */
        e->stats.urcs++;
        if (e->urc_cb)
            e->urc_cb(line, e->urc_arg);
        return 0;
    }

    if (e->resp_count < AT_RESP_MAX) {
        unsigned slot = e->resp_count;

        memcpy(e->resp_store[slot], line,
               strlen(line) + 1);
        e->resp_lines[slot] = e->resp_store[slot];
        e->resp_count++;
    }
    return 0;
}

void at_engine_tick(struct at_engine *e, uint64_t now_ms)
{
    char buf[64];
    int r;

    /* drain transport bytes into lines                             */
    while ((r = e->tp.read(e->tp.priv, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r; i++) {
            char c = buf[i];

            if (c == '\n') {
                e->line[e->line_len] = 0;
                /* strip a trailing CR                             */
                if (e->line_len && e->line[e->line_len - 1] == '\r')
                    e->line[e->line_len - 1] = 0;
                if (e->line_len)
                    line_consume(e, e->line);
                e->line_len = 0;
            } else if (c != '\r' &&
                       e->line_len + 1u < AT_LINE_MAX) {
                e->line[e->line_len++] = c;
            }
            /* the '> ' SMS prompt has no CRLF: dispatch on '>'    */
            if (c == '>' && e->line_len == 1 &&
                e->line[0] == '>' && e->pending) {
                e->line_len = 0;
                line_consume(e, "> ");
            }
        }
    }

    if (e->pending &&
        (int64_t)(now_ms - e->sent_ms) >= (int64_t)e->timeout_ms) {
        if (e->retries_left) {
            e->retries_left--;
            e->sent_ms = now_ms;
            {
                char line[AT_CMD_MAX + 4u];
                unsigned clen = (unsigned)strlen(e->retry_cmd);

                memcpy(line, e->retry_cmd, clen);
                line[clen++] = '\r';
                e->tp.write(e->tp.priv, line, clen);
            }
            return;
        }
        deliver_error(e, AT_TIMEOUT);
    }
}
