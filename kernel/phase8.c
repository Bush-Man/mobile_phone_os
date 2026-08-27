/*
 * phase8.c - IPC/sync subsystem bring-up (phase 8 entry point).
 *
 * Runs from kmain right after phase7_init. Registry initialization
 * touches nothing blocking, so boot context is safe here; everything
 * that parks (the sync/IPC selftest battery and the two-process
 * pipe+shared-memory milestone demo) runs as its own task once the
 * per-cpu schedulers are live -- mirroring drvtest/fstest from
 * phases 6/7.
 *
 * Milestone wiring: the built-in "ipcdemo" ELF is exec'd as a real
 * EL0 process; it forks, and the two processes exchange data over a
 * pipe AND a shared-memory region before reaping cleanly (see
 * userspace/ipcdemo.c and docs/PHASE_8.md).
 */

#include <stdint.h>

#include "ipc.h"
#include "lib.h"
#include "panic.h"
#include "platform.h"
#include "proc.h"
#include "task.h"
#include "unsock.h"

void ipc_selftest_task(void *arg);              /* selftest_ipc.c */

/*
 * The ipctest battery itself verifies process-independent pieces;
 * watching the user demo means waiting for it, so this starter just
 * launches and reports. Reaping belongs to the user-space parent
 * inside the demo (which IS one of the two talking processes).
 */
static void ipcdemo_starter(void *arg)
{
    int pid;

    (void)arg;

    pid = proc_spawn("ipcdemo",
                     (const char *const []){ "ipcdemo", "phase8",
                                             NULL },
                     NULL);
    if (pid < 0)
        kprintf("[demo] ipcdemo spawn failed (%d)\n", pid);
    else
        kprintf("[demo] ipcdemo spawned pid %d\n", pid);

    task_exit();
}

void phase8_init(const struct platform_info *plat)
{
    static bool done;

    (void)plat;

    if (done)
        return;
    done = true;

    ipc_subsys_init();
    usock_subsys_init();
    kprintf("ipc: registries online\n");

    task_create("ipctest", ipc_selftest_task, NULL, 50);
    task_create("ipcdemo", ipcdemo_starter, NULL, 40);
}
