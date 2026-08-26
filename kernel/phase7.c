/*
 * phase7.c - filesystem subsystem bring-up (phase 7 entry point).
 *
 * Runs from kmain right after phase6_init: registers the built-in
 * filesystem types, mounts ramfs as the namespace root ("/") and
 * devfs at "/dev" -- both are non-blocking registry walks, safe in
 * boot context.
 *
 * Disk-backed mounts (vfat/ext2) need blocking block-layer IO, so
 * they happen inside the "fstest" task together with the selftest
 * battery, exactly like drvtest in phase 6.
 */

#include <stdint.h>

#include "devfs.h"
#include "ext2.h"
#include "fat32.h"
#include "lib.h"
#include "platform.h"
#include "ramfs.h"
#include "task.h"
#include "vfs.h"

void fs_selftest_task(void *arg);        /* kernel/selftest_fs.c   */

void phase7_init(const struct platform_info *plat)
{
    static bool done;

    (void)plat;

    if (done)
        return;
    done = true;

    vfs_subsys_init();
    ramfs_init();
    devfs_init();
    fat32_init();
    ext2_init();

    if (vfs_mount("ramfs", "/", NULL, 0, 0))
        panic("phase7: cannot mount root");

    /* best effort until the tty console char device exists         */
    vfs_mount("devfs", "/dev", NULL, 0, 0);

    kprintf("vfs: %u mount%s registered\n",
            vfs_mount_count(), vfs_mount_count() == 1 ? "" : "s");

    task_create("fstest", fs_selftest_task, NULL, 50);
}
