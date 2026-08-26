/*
 * phase6.c - driver subsystem bring-up (phase 6 entry point).
 *
 * Runs from kmain before the banner: registers the built-in driver
 * tables, enumerates devices from the embedded DTB, probes them,
 * hands console echo over to the tty layer, discovers partitions,
 * and spawns the blocking selftest as a task (it needs scheduler
 * context for msleep-based IO waits).
 */

#include <stdint.h>

#include "block.h"
#include "device.h"
#include "fdt.h"
#include "lib.h"
#include "platform.h"
#include "task.h"
#include "tty.h"

extern const uint8_t _binary_platform_qemu_virt_dtb_start[];

void driver_selftest_task(void *arg);   /* kernel/selftest_driver.c */

/* built-in drivers (each file owns its struct) */
extern struct driver virtio_mmio_drv;
extern struct driver pl061_drv;

static struct driver *const builtin_drivers[] = {
    &virtio_mmio_drv,                   /* transports before frontends */
    &pl061_drv,
};

/*
 * NOTE: everything that touches disks (or tty reads) must run from
 * task context -- the IO paths park via msleep()/wait queues, which
 * need a current task. Boot-context bring-up therefore only probes
 * hardware; the "drvtest" task does the rest.
 */
void phase6_init(const struct platform_info *plat)
{
    static bool done;
    struct fdt f;

    if (done)
        return;
    done = true;

    driver_subsys_init();
    for (unsigned i = 0; i < sizeof(builtin_drivers) /
                               sizeof(builtin_drivers[0]); i++)
        driver_register(builtin_drivers[i]);

    if (fdt_init(&f, (uintptr_t)_binary_platform_qemu_virt_dtb_start) == 0) {
        uint64_t claimed = plat->has_uart ? plat->uart_base : ~(uint64_t)0;

        int n = devices_enumerate_fdt(&f, claimed);

        kprintf("drvcore: %d device%s from FDT, probing...\n",
                n, n == 1 ? "" : "s");
        device_probe_all();
        device_dump();
        kprintf("drvcore: %u devices, %u drivers registered\n",
                device_count(), driver_count());
    }

    tty_init();                         /* console handover */

    task_create("drvtest", driver_selftest_task, NULL, 50);
}
