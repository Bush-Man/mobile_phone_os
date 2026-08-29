/*
 * udevd.c - lite device manager (phase 14, plan item 77).
 *
 * Enumerates the device model through SYS_devlist and writes a
 * snapshot to /var/run/devices, then settles into a slow heartbeat.
 * A hotplug-event loop arrives with real hardware (phase 16); what
 * this daemon proves today is that EL0 can consume the kernel's
 * device registry as a report and act as a managed service init
 * restarts if killed.
 */

#include "libc.h"
#include "sysinfo.h"

#define DEVS_MAX 32

int main(int argc, char **argv)
{
    struct dev_info devs[DEVS_MAX];
    int n;

    (void)argc;
    (void)argv;

    n = devlist(devs, DEVS_MAX);
    if (n < 0) {
        printf("udevd: devlist failed (%d)\n", n);
        return 1;
    }

    {
        int fd = open("/var/run/devices",
                      O_WRONLY | O_CREAT | O_TRUNC);

        for (int i = 0; i < n; i++) {
            const char *state = devs[i].state == 1 ? "bound"
                                    : (devs[i].state == 2 ? "failed"
                                                          : "nodrv");

            printf("udevd: %s %s %s\n", devs[i].name,
                   devs[i].drv[0] ? devs[i].drv : "-", state);
            if (fd >= 0) {
                char line[96];

                snprintf(line, sizeof(line), "%s %s %s\n",
                         devs[i].name,
                         devs[i].drv[0] ? devs[i].drv : "-", state);
                write(fd, line, strlen(line));
            }
        }
        if (fd >= 0)
            close(fd);
    }

    printf("udevd: %d devices indexed -> /var/run/devices\n", n);

    for (;;)
        sleep_ms(10000);
    return 0;
}
