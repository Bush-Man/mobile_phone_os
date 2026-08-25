#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

#include "fdt.h"

#define PLATFORM_MODEL_MAX    64
#define PLATFORM_BOOTARGS_MAX 128

struct platform_info {
    char     model[PLATFORM_MODEL_MAX];
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t uart_base;
    char     boot_args[PLATFORM_BOOTARGS_MAX];
    int      has_uart;
    int      has_boot_args;
};

void platform_probe(struct platform_info *pi, const struct fdt *f);
void platform_self(struct platform_info *pi);   /* probe embedded DTB */

#endif /* PLATFORM_H */
