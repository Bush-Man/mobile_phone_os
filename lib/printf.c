/*
 * printf.c - minimal kernel formatter writing straight to the UART.
 * Supported conversions: %c %s %% %d %i %u %x %X %p,
 * with optional 'l' / 'll' length modifiers on integer conversions.
 * No width, precision or float support (kernel keeps FP registers off).
 */

#include <stdarg.h>
#include <stdint.h>

#include "lib.h"
#include "uart.h"

static void put_str(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void put_ull(unsigned long long v, unsigned base, int uppercase)
{
    char buf[24];
    const char *digits = uppercase ? "0123456789ABCDEF"
                                   : "0123456789abcdef";
    int i = 0;

    do {
        buf[i++] = digits[v % base];
        v /= base;
    } while (v);

    while (i--)
        uart_putc(buf[i]);
}

static void put_ll(long long v)
{
    unsigned long long mag = (v < 0) ? -(unsigned long long)v
                                     : (unsigned long long)v;

    if (v < 0)
        uart_putc('-');
    put_ull(mag, 10, 0);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    const char *p;

    va_start(ap, fmt);
    for (p = fmt; *p; p++) {
        int lcount;

        if (*p != '%') {
            uart_putc(*p);
            continue;
        }
        p++;
        lcount = 0;
        while (*p == 'l') {
            lcount++;
            p++;
        }

        switch (*p) {
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 's':
            put_str(va_arg(ap, const char *));
            break;
        case 'd':
        case 'i': {
            long long v = (lcount >= 1) ? va_arg(ap, long long)
                                        : (long long)va_arg(ap, int);
            put_ll(v);
            break;
        }
        case 'u': {
            unsigned long long v =
                (lcount >= 1) ? va_arg(ap, unsigned long long)
                              : (unsigned long long)va_arg(ap, unsigned int);
            put_ull(v, 10, 0);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v =
                (lcount >= 1) ? va_arg(ap, unsigned long long)
                              : (unsigned long long)va_arg(ap, unsigned int);
            put_ull(v, 16, *p == 'X');
            break;
        }
        case 'p':
            put_str("0x");
            put_ull((unsigned long long)(uintptr_t)va_arg(ap, void *),
                    16, 0);
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            uart_putc(*p);
            break;
        }
    }
    va_end(ap);
}
