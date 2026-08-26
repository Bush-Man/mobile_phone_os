#ifndef MMC_H
#define MMC_H

#include <stdint.h>

/*
 * MMC/SD block frontends (phase 6). The SDHCI backend speaks the
 * standard SD Host Controller register interface found on Pi /
 * PinePhone-class boards. QEMU `-M virt` has no SDHCI node, so the
 * driver is wired by board bring-up code, not by FDT probing.
 *
 * Deliberate scope: PIO transfers, polling (no IRQ/ADMA2), 4-bit bus,
 * single-block commands -- correctness over throughput; the request
 * queue above it hides the difference.
 */

int sdhci_register(uintptr_t mmio_base, const char *name);

#endif /* MMC_H */
