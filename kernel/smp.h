#ifndef SMP_H
#define SMP_H

#include <stdint.h>

struct platform_info;

#include "cpu.h"
extern uint64_t sec_stacks[NR_CPUS];
void secondary_entry(void);

void smp_init(void);
void secondary_start(uint64_t cpu) __attribute__((noreturn));

#endif /* SMP_H */
