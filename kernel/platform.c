/*
 * platform.c - extract boot-relevant facts from the device tree into
 * a plain struct other subsystems can consume without knowing about FDT.
 *
 * The QEMU virt DTB is linked into the image as .rodata.fdt by the
 * Makefile (objcopy of platform/qemu-virt.dtb); real boards will hand
 * us the pointer in x1 instead and platform_self() gains a parameter.
 */

#include <stdint.h>
#include <stdbool.h>

#include "fdt.h"
#include "irq.h"
#include "platform.h"

extern const uint8_t _binary_platform_qemu_virt_dtb_start[];
extern const uint8_t _binary_platform_qemu_virt_dtb_end[];

static void copy_str(char *dst, const char *src, int max)
{
    int i = 0;

    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* assemble a u64 from 'cells' consecutive big-endian u32s */
static uint64_t cells_u64(const uint32_t *p, int cells)
{
    uint64_t v = 0;

    for (int i = 0; i < cells && i < 2; i++)
        v = (v << 32) | fdt_u32(p + i);
    return v;
}

/* exact-match helper against one NUL-terminated compatible string */
static bool compat_is(const char *s, int len, const char *name)
{
    while (len > 0 && *name) {
        if (*s++ != *name++)
            return false;
        len--;
    }
    return len == 1 && *s == '\0';      /* consumed incl. terminator */
}

/* bounded string length within a property blob */
static int prop_strlen(const char *s, int max)
{
    int n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

static uint32_t root_cells(const struct fdt *f, const char *prop,
                           uint32_t dflt)
{
    int len;
    const void *v = fdt_getprop(f, fdt_find_node(f, "/"), prop, &len);

    return (v && len == 4) ? fdt_u32(v) : dflt;
}

static void probe_memory(struct platform_info *pi, const struct fdt *f)
{
    int node = fdt_find_node(f, "/memory*");
    int len;
    const void *reg;
    uint32_t ac = root_cells(f, "#address-cells", 2);
    uint32_t sc = root_cells(f, "#size-cells", 2);

    pi->ram_base = 0;
    pi->ram_size = 0;
    if (node < 0)
        return;

    reg = fdt_getprop(f, node, "reg", &len);
    if (!reg || len < (int)(4 * (ac + sc)))
        return;

    pi->ram_base = cells_u64(reg, ac);
    pi->ram_size = cells_u64((const uint32_t *)reg + ac, sc);
}

static void probe_serial(struct platform_info *pi, const struct fdt *f)
{
    /* node placement varies by SoC/firmware: direct child of root on
     * current QEMU virt, under /soc elsewhere */
    static const char *paths[] = {
        "/pl011*", "/soc/pl011*", "/serial*", "/soc/serial*",
    };
    int len;
    const void *reg;

    pi->has_uart = 0;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int node = fdt_find_node(f, paths[i]);

        if (node < 0)
            continue;
        reg = fdt_getprop(f, node, "reg", &len);
        /* need at least #address-cells words for the base */
        if (!reg || len < 4)
            continue;
        pi->uart_base = cells_u64(reg,
                                  (int)root_cells(f, "#address-cells", 1));
        pi->has_uart  = 1;

        /* GIC interrupt specifier: <type number flags>, 3 cells on
         * every GIC binding; type 0 = SPI (+32), type 1 = PPI (+16) */
        reg = fdt_getprop(f, node, "interrupts", &len);
        if (reg && len >= 12) {
            uint32_t type = fdt_u32(reg);
            uint32_t num  = fdt_u32((const uint32_t *)reg + 1);

            pi->uart_irq = (type == 0) ? num + IRQ_SPI_BASE
                         : (type == 1) ? num + IRQ_PPI_BASE
                                       : num;
        }
        return;
    }
}

/* interrupt-controller node: bases, and version from its compatible */
static void probe_intc(struct platform_info *pi, const struct fdt *f)
{
    int node = fdt_find_node(f, "/intc*");
    int len;
    const void *reg;
    const void *compat;
    uint32_t ac = root_cells(f, "#address-cells", 2);
    uint32_t sc = root_cells(f, "#size-cells", 2);

    pi->has_gic = 0;
    pi->gic_version = 0;
    if (node < 0)
        return;

    compat = fdt_getprop(f, node, "compatible", &len);
    for (const char *s = compat; s && len > 0; ) {
        int slen = prop_strlen(s, len) + 1;

        if (!pi->gic_version && (compat_is(s, slen, "arm,cortex-a15-gic") ||
                                 compat_is(s, slen, "arm,gic-400")))
            pi->gic_version = 2;
        else if (compat_is(s, slen, "arm,gic-v3"))
            pi->gic_version = 3;
        len -= slen;
        s += slen;
    }

    /* reg = <gicd-frame><gicc-frame> under the v2 binding */
    reg = fdt_getprop(f, node, "reg", &len);
    if (!reg || len < (int)(4 * 2 * (ac + sc)))
        return;

    pi->gicd_base = cells_u64(reg, ac);
    pi->gicc_base = cells_u64((const uint32_t *)reg + ac + sc, ac);
    pi->has_gic   = pi->gicd_base && pi->gicc_base;
}

/*
 * Architected timer PPIs are fixed by the architecture but the DTB
 * still lists them; cross-check our constant against entry 3 of the
 * standard four-specifier list (sec-phys, ns-phys, virtual, hyp).
 * Falls back to the architectural value when the node is absent.
 */
static void probe_timer_irq(struct platform_info *pi, const struct fdt *f)
{
    int node = fdt_find_node(f, "/timer*");
    int len;
    const void *irqs;

    pi->timer_irq = IRQ_PPI_VIRT_TIMER;
    if (node < 0)
        return;

    irqs = fdt_getprop(f, node, "interrupts", &len);
    if (!irqs || len < (int)(12 * 3))
        return;

    /* third specifier is the virtual timer: <PPI 11 flags> */
    if (fdt_u32((const uint32_t *)irqs + 6) == 1)
        pi->timer_irq = fdt_u32((const uint32_t *)irqs + 7) + IRQ_PPI_BASE;
}

static void probe_chosen(struct platform_info *pi, const struct fdt *f)
{
    int node = fdt_find_node(f, "/chosen");
    int len;
    const void *ba;

    pi->has_boot_args = 0;
    if (node < 0)
        return;

    ba = fdt_getprop(f, node, "bootargs", &len);
    if (ba && len > 0) {
        copy_str(pi->boot_args, ba,
                 len < PLATFORM_BOOTARGS_MAX ? len : PLATFORM_BOOTARGS_MAX);
        pi->has_boot_args = 1;
    }
}

/* /psci: conduit + CPU_ON function id for SMP bring-up */
static void probe_psci(struct platform_info *pi, const struct fdt *f)
{
    int node = fdt_find_node(f, "/psci*");
    int len;
    const void *v;

    pi->has_psci = 0;
    if (node < 0)
        return;

    v = fdt_getprop(f, node, "method", &len);
    if (!v || len < 4)
        return;
    pi->psci_hvc = compat_is(v, len, "hvc") ? 1 : 0;

    v = fdt_getprop(f, node, "cpu_on", &len);
    if (!v || len != 4)
        return;
    pi->psci_cpu_on_fn = fdt_u32(v);
    pi->has_psci = 1;
}

void platform_probe(struct platform_info *pi, const struct fdt *f)
{
    int root = fdt_find_node(f, "/");
    int len;
    const void *model;

    copy_str(pi->model, "", PLATFORM_MODEL_MAX);
    if (root >= 0) {
        model = fdt_getprop(f, root, "model", &len);
        if (model && len > 0)
            copy_str(pi->model, model, PLATFORM_MODEL_MAX);
    }

    probe_memory(pi, f);
    probe_serial(pi, f);
    probe_intc(pi, f);
    probe_timer_irq(pi, f);
    probe_psci(pi, f);
    probe_chosen(pi, f);
}

void platform_self(struct platform_info *pi)
{
    struct fdt f;

    if (fdt_init(&f, (uintptr_t)_binary_platform_qemu_virt_dtb_start) != 0) {
        copy_str(pi->model, "<bad dtb>", PLATFORM_MODEL_MAX);
        pi->ram_base = 0;
        pi->ram_size = 0;
        pi->uart_base = 0;
        pi->gicd_base = 0;
        pi->gicc_base = 0;
        pi->has_uart = 0;
        pi->has_boot_args = 0;
        pi->has_gic = 0;
        pi->gic_version = 0;
        pi->uart_irq = 0;
        pi->timer_irq = IRQ_PPI_VIRT_TIMER;
        pi->has_psci = 0;
        pi->psci_hvc = 0;
        pi->psci_cpu_on_fn = 0;
        return;
    }
    platform_probe(pi, &f);
}
