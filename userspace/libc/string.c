/*
 * string.c - freestanding string/memory routines (phase 14).
 *
 * Plain, deterministic C: GCC would otherwise emit calls to these
 * same names, so they must exist with exact C semantics.
 */

#include "libc.h"

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

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
    const unsigned char *pa = a;
    const unsigned char *pb = b;

    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;

    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--)
        *d++ = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;

    while (*d)
        d++;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    for (;; s++) {
        if (*s == ch)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

unsigned long strtoul(const char *s, const char **end_out, int base)
{
    unsigned long v = 0;
    int neg = 0, any = 0;

    if (base != 10 && base != 16)
        base = 10;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;

    for (;; s++) {
        int dgt;

        if (*s >= '0' && *s <= '9')
            dgt = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            dgt = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            dgt = *s - 'A' + 10;
        else
            break;
        if (dgt >= base)
            break;
        v = v * (unsigned long)base + (unsigned long)dgt;
        any = 1;
    }

    if (end_out)
        *end_out = any ? s : s - 1;
    return neg ? (unsigned long)(-(long)v) : v;
}
