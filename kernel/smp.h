#ifndef SMP_H
#define SMP_H

#include <stdint.h>

void smp_init(void);
void secondary_start(uint64_t cpu) __attribute__((noreturn));

#endif /* SMP_H */
