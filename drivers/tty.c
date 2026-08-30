/*
 * tty.c - serial console line discipline + "console" char device.
 *
 * Byte flow:  PL011 IRQ -> uart rx ring -> (tasklet) tty_rx_byte ->
 *             canonical editor / raw passthrough -> line queue ->
 *             blocking readers (tty_read).
 *
 * Echo is local here once attached, so erase/kill render correctly
 * ("\b \b"), CR normalizes to CRLF, and ^C/^D echo as visible marks.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "chardev.h"
#include "lib.h"
#include "spinlock.h"
#include "task.h"
#include "tasklet.h"
#include "tty.h"
#include "uart.h"

struct tty ttys0 = {
    .canon = true,
    .echo  = true,
    .lock  = SPINLOCK_INIT,
};

/* ---- queue helpers --------------------------------------------------------- */

static bool q_empty(const struct tty *t)
{
    return t->q_head == t->q_tail;
}

static bool q_full(const struct tty *t)
{
    return ((t->q_tail + 1u) % TTY_QUEUED_MAX) == t->q_head;
}

/* enqueue a completed line; drops on overflow (counted) */
static void enqueue_line(struct tty *t, const char *data, unsigned len)
{
    unsigned slot;

    if (q_full(t)) {
        t->stats.overflow_drops++;
        return;
    }

    if (len > TTY_LINE_MAX - 1)
        len = TTY_LINE_MAX - 1;

    slot = t->q_tail;
    for (unsigned i = 0; i < len; i++)
        t->q[slot][i] = data[i];
    t->q[slot][len] = '\n';     /* always newline-terminated storage */
    t->q_tail = (slot + 1u) % TTY_QUEUED_MAX;
    t->stats.lines_done++;
}

/* dequeue one stored line; returns its length */
static unsigned dequeue_line(struct tty *t, char *dst, unsigned max)
{
    unsigned slot = t->q_head;
    const char *src = t->q[slot];
    unsigned len = 0;

    while (src[len] && src[len] != '\n')
        len++;
    if (src[len] == '\n')
        len++;                  /* include the newline */

    if (len > max)
        len = max;
    for (unsigned i = 0; i < len; i++)
        dst[i] = src[i];

    t->q_head = (slot + 1u) % TTY_QUEUED_MAX;
    return len;
}

/* ---- output ------------------------------------------------------------------ */

static int tty_emit(const char *src, unsigned n)
{
    daif_state s;
    unsigned written = 0;

    uart_tx_begin(&s);
    while (written < n) {
        char c = src[written++];

        uart_putc(c);
        if (c == '\n')
            ttys0.stats.tx_chars++;     /* count the implied \r too */
        ttys0.stats.tx_chars++;
    }
    uart_tx_end(s);
    return (int)n;
}

int tty_write(const char *src, unsigned n)
{
    return tty_emit(src, n);
}

int tty_puts(const char *s)
{
    unsigned n = 0;

    while (s[n])
        n++;
    return tty_emit(s, n);
}

/* ---- input side ----------------------------------------------------------------- */

/* wait_sleep_when() parks while the predicate is TRUE */
static bool reader_must_wait(void *ctx)
{
    struct tty *t = ctx;

    return q_empty(t);
}

static void wake_readers(struct tty *t)
{
    wait_wake_all(&t->rdq);
}

/* process one byte through the discipline; may emit echo */
static void tty_rx_byte(struct tty *t, char c)
{
    uint8_t b = (uint8_t)c;
    daif_state s;

    s = irq_local_save();
    spin_lock(&t->lock);
    t->stats.rx_chars++;

    if (!t->canon) {
        /* raw mode: every byte becomes an immediately-readable unit */
        enqueue_line(t, &c, 1);
        wake_readers(t);
        spin_unlock(&t->lock);
        irq_local_restore(s);
        return;
    }

    switch (b) {
    case '\r':
    case '\n':
        if (t->echo)
            tty_emit("\r\n", 2);
        enqueue_line(t, t->cur, t->cur_len);
        t->cur_len = 0;
        wake_readers(t);
        break;

    case 0x08:                          /* BS */
    case 0x7f:                          /* DEL */
        if (t->cur_len > 0) {
            t->cur_len--;
            t->stats.erases++;
            if (t->echo)
                tty_emit("\b \b", 3);
        }
        break;

    case 0x15:                          /* ^U: kill line */
        while (t->cur_len > 0) {
            if (t->echo)
                tty_emit("\b \b", 3);
            t->cur_len--;
        }
        t->stats.kills++;
        break;

    case 0x03:                          /* ^C: flush + notice */
        t->cur_len = 0;
        if (t->echo)
            tty_emit("^C", 2);
        break;

    case 0x04:                          /* ^D: EOF -- deliver partials */
        enqueue_line(t, t->cur, t->cur_len);
        t->cur_len = 0;
        wake_readers(t);
        break;

    default:
        if (b >= 0x20 && b < 0x7f) {
            if (t->cur_len < TTY_LINE_MAX - 1) {
                t->cur[t->cur_len++] = c;
                if (t->echo)
                    tty_emit(&c, 1);
            } else {
                if (t->echo)
                    tty_emit("\a", 1);  /* bell: line full */
                t->stats.overflow_drops++;
            }
        }
        /* other control bytes are silently ignored */
        break;
    }

    spin_unlock(&t->lock);
    irq_local_restore(s);
}

/* ---- tasklet glue to the uart ring ---------------------------------------------- */

static void tty_rx_tasklet(void *arg)
{
    char buf[32];
    unsigned n;

    (void)arg;
    while ((n = uart_rx_read(buf, sizeof(buf))) > 0)
        for (unsigned i = 0; i < n; i++)
            tty_rx_byte(&ttys0, buf[i]);
}

static void tty_uart_notify(void)
{
    tasklet_schedule(tty_rx_tasklet, NULL);
}

/* ---- reader API -------------------------------------------------------------------- */

unsigned tty_lines_pending(void)
{
    daif_state s = irq_local_save();
    unsigned n = (ttys0.q_tail - ttys0.q_head) % TTY_QUEUED_MAX;

    irq_local_restore(s);
    return n;
}

int tty_read(char *dst, unsigned max)
{
    wait_sleep_when(reader_must_wait, &ttys0, &ttys0.rdq);

    daif_state s = irq_local_save();
    int r;

    spin_lock(&ttys0.lock);
    if (q_empty(&ttys0)) {
        r = 0;                          /* spurious wakeup */
    } else {
        r = (int)dequeue_line(&ttys0, dst, max);
    }
    spin_unlock(&ttys0.lock);
    irq_local_restore(s);
    return r;
}

void tty_set_canon(bool canon)
{
    daif_state s = irq_local_save();

    ttys0.canon = canon;
    irq_local_restore(s);
}

struct tty_stats tty_get_stats(void)
{
    return ttys0.stats;
}

/* ---- selftest injection --------------------------------------------------------------- */

void tty_test_feed(const char *bytes)
{
    while (*bytes)
        tty_rx_byte(&ttys0, *bytes++);
}

/* ---- attach ------------------------------------------------------------------------------ */

static int tty_dev_read(struct char_dev *cd, char *dst, unsigned max)
{
    (void)cd;
    return tty_read(dst, max);
}

static int tty_dev_write(struct char_dev *cd, const char *src, unsigned n)
{
    (void)cd;
    return tty_write(src, n);
}

static unsigned tty_dev_poll(struct char_dev *cd)
{
    (void)cd;
    return tty_lines_pending();
}

static struct char_dev console_chardev = {
    .name  = "console",
    .read  = tty_dev_read,
    .write = tty_dev_write,
    .poll  = tty_dev_poll,
};

void tty_init(void)
{
    static bool done;
    char scratch[16];
    unsigned n;

    if (done)
        return;
    done = true;

    /* take over echo duty from the early-boot raw path */
    uart_echo_set(false);

    /* anything already sitting in the ring enters via the discipline */
    while ((n = uart_rx_read(scratch, sizeof(scratch))) > 0)
        for (unsigned i = 0; i < n; i++)
            tty_rx_byte(&ttys0, scratch[i]);

    uart_rx_notify(tty_uart_notify);

    /* close the arm race: pushes after arming notify us directly */
    while ((n = uart_rx_read(scratch, sizeof(scratch))) > 0)
        for (unsigned i = 0; i < n; i++)
            tty_rx_byte(&ttys0, scratch[i]);

    char_dev_register(&console_chardev);

    kprintf("tty: console online (canonical mode, local echo)\n");
}
