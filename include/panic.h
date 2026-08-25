#ifndef PANIC_H
#define PANIC_H

void panic(const char *msg) __attribute__((noreturn));
void __stack_chk_fail(void) __attribute__((noreturn));

#endif /* PANIC_H */
