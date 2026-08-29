#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

void panic(const char *msg) __attribute__((noreturn));
void __stack_chk_fail(void) __attribute__((noreturn));

/* phase 16: the guard ships with a fixed value in panic.c so very
 * early boot stays deterministic, then kmain re-randomizes it from
 * the architected counter once time is up                          */
extern uintptr_t __stack_chk_guard;

#endif /* PANIC_H */
