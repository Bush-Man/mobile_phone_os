/*
 * stdio.c - printf family over the write() path (phase 14).
 *
 * One formatting engine, two sinks: fd 1 (vprintf/printf) and a
 * caller buffer (vsnprintf/snprintf). Supported conversions: %c %s
 * %% %d %i %u %x %X %p with '0' flag, width digits and l/ll/z
 * length modifiers -- the same subset the kernel's kprintf speaks,
 * so messages look identical on both sides of the boundary.
 */

#include "libc.h"

struct sink {
    char *buf;                  /* NULL -> fd writes                 */
    size_t cap, pos;
    int fd;
    int err;
};

static void sink_putc(struct sink *s, char c)
{
    if (s->buf) {
        if (s->pos + 1 < s->cap)
            s->buf[s->pos] = c;
        s->pos++;
        return;
    }
    write(s->fd, &c, 1);
}

static void sink_puts(struct sink *s, const char *str, size_t len)
{
    if (s->buf) {
        size_t room = s->cap > s->pos + 1 ? s->cap - s->pos - 1 : 0;

        if (len > room)
            len = room;
        for (size_t i = 0; i < len; i++)
            s->buf[s->pos++] = str[i];
        return;
    }
    write(s->fd, str, len);
}

/* emit str, padded to width with 'padc' ('0' flag pads numbers)   */
static void pad_emit(struct sink *s, const char *str, size_t len,
                     int width, char padc)
{
    while ((int)len < width--) {
        sink_putc(s, padc);
    }
    sink_puts(s, str, len);
}

static void emit_u64(struct sink *s, u64 v, unsigned base,
                     int upper, int width, char padc, int neg)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v && n < (int)sizeof(tmp));

    if (neg)
        sink_putc(s, '-');
    pad_emit(s, tmp, (size_t)n, width, padc);
}

static void emit_i64(struct sink *s, i64 v, unsigned base,
                     int upper, int width, char padc)
{
    u64 m;

    if (v < 0) {
        m = (u64)(-(v + 1)) + 1u;
        if (width > 0)
            width--;
        emit_u64(s, m, base, upper, width, padc, 1);
    } else {
        emit_u64(s, (u64)v, base, upper, width, padc, 0);
    }
}

static void emit_ptr(struct sink *s, const void *p)
{
    sink_puts(s, "0x", 2);
    emit_u64(s, (u64)(uintptr_t)p, 16, 0, 16, '0', 0);
}

static void vformat(struct sink *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_putc(s, *fmt);
            continue;
        }
        fmt++;

        char padc = ' ';
        int width = 0, lcount = 0;

        if (*fmt == '0') {
            padc = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l' || *fmt == 'z') {
            lcount++;
            fmt++;
        }

        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(ap, int);

            pad_emit(s, &c, 1, width, ' ');
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);

            pad_emit(s, str ? str : "(null)",
                     strlen(str ? str : "(null)"), width, ' ');
            break;
        }
        case 'd':
        case 'i': {
            i64 v = lcount ? va_arg(ap, i64)
                           : (i64)va_arg(ap, int);

            emit_i64(s, v, 10, 0, width, padc);
            break;
        }
        case 'u': {
            u64 v = lcount ? va_arg(ap, u64)
                           : (u64)va_arg(ap, unsigned int);

            emit_u64(s, v, 10, 0, width, padc, 0);
            break;
        }
        case 'x':
        case 'X': {
            u64 v = lcount ? va_arg(ap, u64)
                           : (u64)va_arg(ap, unsigned int);

            emit_u64(s, v, 16, *fmt == 'X', width, padc, 0);
            break;
        }
        case 'p':
            emit_ptr(s, va_arg(ap, void *));
            break;
        case '%':
            sink_putc(s, '%');
            break;
        case 0:
            return;
        default:
            sink_putc(s, '%');
            sink_putc(s, *fmt);
            break;
        }
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct sink s = { buf, cap, 0, 1, 0 };

    vformat(&s, fmt, ap);
    if (buf && cap)
        buf[s.pos < cap ? s.pos : cap - 1] = 0;
    return (int)s.pos;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap)
{
    struct sink s = { NULL, 0, 0, 1, 0 };

    vformat(&s, fmt, ap);
    return (int)s.pos;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s)
{
    size_t n = strlen(s);

    write(1, s, n);
    write(1, "\n", 1);
    return (int)n + 1;
}

int putchar(int c)
{
    char ch = (char)c;

    write(1, &ch, 1);
    return (unsigned char)ch;
}
