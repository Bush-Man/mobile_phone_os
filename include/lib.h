#ifndef LIB_H
#define LIB_H

#include <stddef.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* freestanding memory routines (GCC emits calls to these) */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);

#endif /* LIB_H */
