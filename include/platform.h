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
    uint64_t gicd_base;         /* interrupt controller distributor  */
    uint64_t gicc_base;         /* ... cpu interface (GICv2)         */
    char     boot_args[PLATFORM_BOOTARGS_MAX];
    int      has_uart;
    int      has_boot_args;
    int      has_gic;
    int      gic_version;       /* probed from compatible string     */
    unsigned uart_irq;          /* console RX line, GIC intid        */
    unsigned timer_irq;         /* architected virtual timer PPI     */
};

void platform_probe(struct platform_info *pi, const struct fdt *f);
void platform_self(struct platform_info *pi);   /* probe embedded DTB */

#endif /* PLATFORM_H */
