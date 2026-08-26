#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);

/*
 * RX-interrupt mode: register the console line with the IRQ framework,
 * enable RX + receive-timeout interrupts and echo received characters
 * from a tasklet (top half only moves bytes into a ring buffer).
 */
void uart_rx_irq_init(unsigned intid);

#endif /* UART_H */
