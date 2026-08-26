#ifndef LIB_H
#define LIB_H

#include <stddef.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* freestanding memory routines (GCC emits calls to these) */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* string helpers (phase 7: filesystems are name-heavy) */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

/*
 * Bounded copy: always NUL-terminates within cap (cap 0 stores
 * nothing), returns the length of src so truncation is detectable.
 */
size_t kstrlcpy(char *dst, const char *src, size_t cap);

#endif /* LIB_H */
