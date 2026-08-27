/*
 * phase10.c - power management & battery bring-up (phase 10 entry).
 *
 * Boot-context work only: PMIC probe over existing I2C adapters
 * (which QEMU does not instantiate), then the battery registry
 * decides -- real gauge online, or the QEMU mock provider for
 * headless percentage reporting. No dedicated PM tasks exist:
 * sampling, warnings and display-suspend policy all run from the
 * housekeeping loop with internal throttling, so the subsystem
 * costs zero scheduler slots.
 */

#include <stdint.h>

#include "battery.h"
#include "lib.h"
#include "platform.h"
#include "task.h"

void pm_selftest_task(void *arg);       /* kernel/selftest_pm.c      */
void pmic_axp_probe(void);

void phase10_init(const struct platform_info *plat)
{
    static bool done;

    if (done)
        return;
    done = true;

    pmic_axp_probe();                   /* item 55: real gauge first  */
    battery_subsys_init(plat);          /* /dev/battery + mock fallback*/

    if (battery_active())
        kprintf("pm10: gauge provider attached (%s)\n",
                battery_active()->name ? battery_active()->name : "?");
    else
        kprintf("pm10: no gauge provider\n");

    task_create("pmtest", pm_selftest_task, NULL, 55);
}
