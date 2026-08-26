/*
 * uart.c - PL011 primecell UART driver.
 * QEMU virt machine maps the first PL011 at 0x09000000 with a 24 MHz clock.
 */

#include <stdint.h>
#include <stddef.h>

#include "irq.h"
#include "mmio.h"
#include "panic.h"
#include "spinlock.h"
#include "tasklet.h"
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
#define UART_MIS    0x40    /* masked interrupt status (read-only) */

#define FR_BUSY (1u << 3)
#define FR_RXFE (1u << 4)   /* RX FIFO empty */
#define FR_TXFF (1u << 5)   /* TX FIFO full */

#define CR_UARTEN (1u << 0)
#define CR_TXE    (1u << 1)
#define CR_RXE    (1u << 2)

#define LCRH_FEN   (1u << 4)    /* enable FIFOs */
#define LCRH_WLEN8 (3u << 5)    /* 8 data bits, no parity, 1 stop */

/* MIS/IMSC bits: RX = FIFO reached threshold, RT = chars idle in FIFO */
#define INT_RX    (1u << 4)
#define INT_RT    (1u << 6)

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

/*
 * TX serialization: multi-line output (kprintf, echo tasklet) holds
 * one spinlock for the whole call so concurrent cpus cannot interleave
 * characters. Callers carry the DAIF state between begin/end.
 */
static spinlock_t tx_lock = SPINLOCK_INIT;

void uart_tx_begin(daif_state *s)
{
    *s = irq_local_save();
    spin_lock(&tx_lock);
}

void uart_tx_end(daif_state s)
{
    spin_unlock(&tx_lock);
    irq_local_restore(s);
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

/* ---- interrupt-driven RX console ---------------------------------------- */

/*
 * Top half only moves bytes out of the FIFO into a small ring; the
 * echo itself (slow: polled TX) runs as a bottom-half tasklet. Both
 * ends mask IRQs around the ring cursors because the producer is IRQ
 * context while the consumer drains in the main loop.
 */
#define RXBUF_SIZE 128

static volatile char     rxbuf[RXBUF_SIZE];
static volatile unsigned rx_head, rx_tail;

static bool rx_push(char c)
{
    daif_state s = irq_local_save();
    unsigned next = (rx_tail + 1u) % RXBUF_SIZE;
    bool ok = next != rx_head;

    if (ok) {
        rxbuf[rx_tail] = c;
        rx_tail = next;
    }                           /* full: drop the byte, UART keeps going */
    irq_local_restore(s);
    return ok;
}

static bool rx_pop(char *out)
{
    daif_state s = irq_local_save();
    bool got = rx_head != rx_tail;

    if (got) {
        *out = rxbuf[rx_head];
        rx_head = (rx_head + 1u) % RXBUF_SIZE;
    }
    irq_local_restore(s);
    return got;
}

static void echo_tasklet(void *arg)
{
    char c;
    daif_state s;

    (void)arg;
    uart_tx_begin(&s);
    while (rx_pop(&c)) {
        uart_putc(c);                       /* raw echo               */
        if (c == '\r')
            uart_putc('\n');                /* CR -> CRLF for logs    */
    }
    uart_tx_end(s);
}

static bool uart_rx_irq(void *arg)
{
    uint32_t mis = mmio_read32(UART0_BASE + UART_MIS);
    bool any = false;

    (void)arg;
    if (!(mis & (INT_RX | INT_RT)))
        return false;                       /* not ours after all */

    /* drain the FIFO: reading DR clears both RX and timeout asserts */
    while (!(mmio_read32(UART0_BASE + UART_FR) & FR_RXFE)) {
        char c = (char)(mmio_read32(UART0_BASE + UART_DR) & 0xffu);

        rx_push(c);
        any = true;
    }

    if (any) {
        tasklet_schedule(echo_tasklet, NULL);
    }
    return true;
}

void uart_rx_irq_init(unsigned intid)
{
    /*
     * Input may have raced us: characters can sit in the RX FIFO
     * from before this driver armed interrupts (firmware/early-boot
     * typed-ahead bytes). Flush them through the normal echo path so
     * nothing typed early is lost.
     */
    while (!(mmio_read32(UART0_BASE + UART_FR) & FR_RXFE)) {
        char c = (char)(mmio_read32(UART0_BASE + UART_DR) & 0xffu);

        rx_push(c);
    }
    tasklet_schedule(echo_tasklet, NULL);

    if (!irq_register(intid, "pl011-rx", uart_rx_irq, NULL))
        panic("uart: rx line already claimed");

    irq_set_priority(intid, 0x80);          /* below the timer tick */
    mmio_write32(UART0_BASE + UART_ICR, INT_RT | INT_RX);   /* stale */
    mmio_write32(UART0_BASE + UART_IMSC, INT_RX | INT_RT);
    irq_enable(intid);
}
