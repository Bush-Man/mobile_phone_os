#ifndef UART_H
#define UART_H

#include <stdint.h>

#include "irq.h"

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);

/* whole-output serialization (SMP-safe lines); caller carries state */
void uart_tx_begin(daif_state *s);
void uart_tx_end(daif_state s);
void uart_panic_mode(void);     /* print without locks from fault paths */

/*
 * RX-interrupt mode: register the console line with the IRQ framework,
 * enable RX + receive-timeout interrupts and echo received characters
 * from a tasklet (top half only moves bytes into a ring buffer).
 */
void uart_rx_irq_init(unsigned intid);

/* kernel-side drain of the RX ring (SYS_read); returns bytes taken */
unsigned uart_rx_read(char *dst, unsigned max);

/* ---- phase 6 hooks (tty ownership handover) ------------------------------ */

/* disable/enable the built-in raw echo (tty takes over when attached) */
void uart_echo_set(bool on);

/* called from the rx top half after each batch of bytes lands in the
 * ring; when set, the raw echo tasklet is bypassed entirely */
void uart_rx_notify(void (*fn)(void));

#endif /* UART_H */
