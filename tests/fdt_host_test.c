/* Host-side harness for lib/fdt.c debugging (not part of the kernel). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fdt.h"

int main(int argc, char **argv)
{
    FILE *fp = fopen(argv[1], "rb");
    static uint8_t buf[4 << 20];
    size_t n;
    struct fdt f;

    n = fread(buf, 1, sizeof(buf), fp);
    printf("file bytes: %zu\n", n);

    if (fdt_init(&f, (uintptr_t)buf) != 0) {
        printf("BAD MAGIC\n");
        return 1;
    }
    printf("totalsize=%u struct_off=%u strings_off=%u version=%u\n",
           fdt_u32(&f.hdr->totalsize),
           fdt_u32(&f.hdr->off_dt_struct),
           fdt_u32(&f.hdr->off_dt_strings),
           fdt_u32(&f.hdr->version));

    int root = fdt_find_node(&f, "/");
    printf("root=%d\n", root);
    const void *model = fdt_getprop(&f, root, "model", NULL);
    printf("model=%s\n", model ? (const char *)model : "(null)");

    int mem = fdt_find_node(&f, "/memory*");
    printf("memory node=%d\n", mem);
    if (mem >= 0) {
        int len;
        const uint32_t *reg = fdt_getprop(&f, mem, "reg", &len);
        printf("mem reg len=%d base=%#llx size=%#llx\n", len,
               reg ? (unsigned long long)((fdt_u32(reg) ? 0 : 0)) : 0,
               0ull);
        if (reg)
            printf("cells: %08x %08x %08x %08x\n",
                   fdt_u32(reg), fdt_u32(reg+1),
                   fdt_u32(reg+2), fdt_u32(reg+3));
    }

    int chos = fdt_find_node(&f, "/chosen");
    printf("chosen=%d\n", chos);

    int soc = fdt_find_node(&f, "/soc");
    printf("soc=%d\n", soc);
    int uart = fdt_find_node(&f, "/soc/pl011*");
    printf("pl011=%d\n", uart);
    if (uart >= 0) {
        int len;
        const uint32_t *reg = fdt_getprop(&f, uart, "reg", &len);
        printf("uart reg len=%d base=%#x\n", len, reg ? fdt_u32(reg) : 0);
    }
    return 0;
}
