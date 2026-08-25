/*
 * uart.c - PL011 primecell UART driver.
 * QEMU virt machine maps the first PL011 at 0x09000000 with a 24 MHz clock.
 */

#include <stdint.h>

#include "mmio.h"
#include "uart.h"

#define UART0_BASE  0x09000000UL

#define UART_DR     0x00    /* data register */
#define UART_FR     0x18    /* flag register */
#define UART_IBRD   0x24    /* integer baud rate divisor */
#define UART_FBRD   0x28    /* fractional baud rate divisor */
#define UART_LCRH   0x2c    /* line control */
#define UART_CR     0x30    /* control */
#define UART_IMSC   0x38    /* interrupt mask */
#define UART_ICR    0x44    /* interrupt clear */

#define FR_BUSY (1u << 3)
#define FR_RXFE (1u << 4)   /* RX FIFO empty */
#define FR_TXFF (1u << 5)   /* TX FIFO full */

#define CR_UARTEN (1u << 0)
#define CR_TXE    (1u << 1)
#define CR_RXE    (1u << 2)

#define LCRH_FEN   (1u << 4)    /* enable FIFOs */
#define LCRH_WLEN8 (3u << 5)    /* 8 data bits, no parity, 1 stop */

void uart_init(void)
{
    mmio_write32(UART0_BASE + UART_IMSC, 0);        /* mask all IRQs      */
    mmio_write32(UART0_BASE + UART_ICR, 0x7ff);     /* clear all pending  */

    /* baud divisor for 115200 from a 24 MHz clock:
     * BAUDDIV = 24e6 / (16 * 115200) = 13.0208 -> IBRD=13, FBRD=1 */
    mmio_write32(UART0_BASE + UART_IBRD, 13);
    mmio_write32(UART0_BASE + UART_FBRD, 1);

    mmio_write32(UART0_BASE + UART_LCRH, LCRH_FEN | LCRH_WLEN8);
    mmio_write32(UART0_BASE + UART_CR,
                 CR_UARTEN | CR_TXE | CR_RXE);
}

void uart_putc(char c)
{
    if (c == '\n') {
        while (mmio_read32(UART0_BASE + UART_FR) & FR_TXFF)
            ;
        mmio_write32(UART0_BASE + UART_DR, '\r');
    }
    while (mmio_read32(UART0_BASE + UART_FR) & FR_TXFF)
        ;
    mmio_write32(UART0_BASE + UART_DR, (uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

char uart_getc(void)
{
    while (mmio_read32(UART0_BASE + UART_FR) & FR_RXFE)
        ;
    return (char)(mmio_read32(UART0_BASE + UART_DR) & 0xffu);
}
