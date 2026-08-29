/*
 * string.c - the handful of libc memory routines GCC emits calls to
 * even under -ffreestanding (e.g. __builtin_memset with a variable
 * size lowers to memset). Phase 7 added the str* helpers the
 * filesystem code needs (lib had none until now; subsystems used to
 * grow private copies -- see p_strlen in kernel/proc.c).
 */

#include <stddef.h>
#include <stdint.h>

#include "lib.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || !n)
        return dst;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;

    for (; n; n--, x++, y++)
        if (*x != *y)
            return (int)*x - (int)*y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (int)(uint8_t)*a - (int)(uint8_t)*b;
        if (!*a)
            break;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t nlen;

    if (!*needle)
        return (char *)hay;
    nlen = strlen(needle);
    for (; *hay; hay++)
        if (strncmp(hay, needle, nlen) == 0)
            return (char *)hay;
    return NULL;
}

size_t kstrlcpy(char *dst, const char *src, size_t cap)
{
    size_t slen = strlen(src);

    if (cap) {
        size_t copy = slen < cap - 1 ? slen : cap - 1;

        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return slen;
}
