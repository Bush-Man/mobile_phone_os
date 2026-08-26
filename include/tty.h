#ifndef TTY_H
#define TTY_H

#include <stdbool.h>
#include <stdint.h>

#include "spinlock.h"
#include "task.h"

/*
 * tty - serial console line discipline (phase 6).
 *
 * Sits between the PL011 RX ring (raw bytes, IRQ-fed) and console
 * readers. Canonical mode accumulates a line with full editing
 * (backspace/del erase, ^U kill, ^C flush, ^D EOF) and local echo;
 * completed lines queue for readers who block until one arrives.
 *
 * On attach it takes over echo duty from drivers/uart.c (which keeps
 * only moving bytes into its ring).
 */

#define TTY_LINE_MAX   128         /* longest editable line          */
#define TTY_QUEUED_MAX 8           /* completed lines held for readers */

struct tty_stats {
    uint64_t rx_chars;
    uint64_t tx_chars;
    uint64_t lines_done;
    uint64_t erases;
    uint64_t kills;
    uint64_t overflow_drops;
};

struct tty {
    char cur[TTY_LINE_MAX];     /* line being edited               */
    unsigned cur_len;

    char q[TTY_QUEUED_MAX][TTY_LINE_MAX];
    volatile unsigned q_head, q_tail;       /* completed-line fifo */

    bool canon;                 /* line mode (vs raw byte mode)    */
    bool echo;
    bool attached;

    spinlock_t lock;
    struct waitqueue rdq;

    struct tty_stats stats;
};

extern struct tty ttys0;

void tty_init(void);            /* attach to uart rx + register dev */

int  tty_read(char *dst, unsigned max);     /* blocking, one line  */
int  tty_write(const char *src, unsigned n);
int  tty_puts(const char *s);

unsigned tty_lines_pending(void);
void tty_set_canon(bool canon);
struct tty_stats tty_get_stats(void);

/* selftest-only injection: feed bytes through the discipline */
void tty_test_feed(const char *bytes);

#endif /* TTY_H */
