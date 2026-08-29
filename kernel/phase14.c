/*
 * phase14.c - userspace foundation bring-up (phase 14 entry).
 *
 * Arms the crash-record subsystem (RAM ring; the VFS path opens
 * lazily on first record), then spawns the "usertest" kernel-task
 * battery and -- last, so all reporting surfaces exist first --
 * the "init" process, registering it as PID 1 / orphan reaper.
 * init itself mounts the /var scaffolding, starts the daemons and
 * the shell, and restarts any critical daemon that dies.
 */

#include <stdint.h>

#include "crash.h"
#include "lib.h"
#include "platform.h"
#include "proc.h"
#include "task.h"

void userspace_selftest_task(void *arg);    /* kernel/selftest_userspace.c */

void phase14_init(const struct platform_info *plat)
{
    static bool done;
    int init_pid;

    (void)plat;
    if (done)
        return;
    done = true;

    crash_init();

    /*
     * The battery first: it waits for init to exist, then drives
     * libctest/crasher and the daemon-respawn check against it.
     */
    task_create("usertest", userspace_selftest_task, NULL, 58);

    init_pid = proc_spawn("init",
                          (const char *const []){ "init", NULL },
                          NULL);
    if (init_pid > 0) {
        proc_note_init_pid(init_pid);
    } else {
        kprintf("[phase14] init spawn failed (%d)\n", init_pid);
    }
}
