/*
 * phase15.c - UI framework & phone apps bring-up (phase 15 entry).
 *
 * The heavy lifting is elsewhere: the compositor/app binaries are
 * EL0 programs init spawns (proc.c builtins), the telephony
 * broker rides phase12_init (modemd), and the presentation ioctls
 * are fb0's (syscall.c). This entry only arms the "uitest15"
 * battery, which waits for init + compositor to exist and then
 * drives the end-to-end milestone (unlock -> dialer -> call
 * events; SMS notification) -- see kernel/selftest_ui.c.
 */

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "task.h"

void ui_selftest_task(void *arg);       /* kernel/selftest_ui.c   */

void phase15_init(const struct platform_info *plat)
{
    static bool done;

    (void)plat;
    if (done)
        return;
    done = true;

    task_create("uitest15", ui_selftest_task, NULL, 54);
}
