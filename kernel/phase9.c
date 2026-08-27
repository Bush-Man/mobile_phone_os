/*
 * phase9.c - graphics & input subsystem bring-up (phase 9 entry).
 *
 * Boot-context work is registration only: backends enter the fb
 * registry, /dev/event0 enters the chardev registry, buttons check
 * their board pin table. Everything blocking (GPU command
 * round-trips during resource arming) happens later inside gfxtest,
 * which also pushes the calibration stream and spawns the evreader
 * process for the milestone proof.
 */

#include <stdint.h>

#include "buttons.h"
#include "fb.h"
#include "input.h"
#include "lib.h"
#include "platform.h"
#include "task.h"

void gfx_selftest_task(void *arg);      /* kernel/selftest_gfx.c     */
void fb_virtio_gpu_backend_register(void);

void phase9_init(const struct platform_info *plat)
{
    static bool done;

    if (done)
        return;
    done = true;

    fb_virtio_gpu_backend_register();   /* display backend           */

    input_subsys_init();                /* /dev/event0 stream core   */
    buttons_subsys_init(plat);          /* board keys if any         */

    kprintf("gfx/input: registries online\n");

    task_create("gfxtest", gfx_selftest_task, NULL, 45);
}
