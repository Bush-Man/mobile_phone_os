/*
 * selftest_userspace.c - the phase-14 battery ("usertest").
 *
 * Runs as a kernel task; every check drives the real userspace:
 *
 *   1. libctest  -- spawn the libc battery and reap exit code 0
 *                   (string/printf/malloc/brk/pthread all green).
 *   2. crasher   -- spawn the deliberate fault; reap it and assert
 *                   a crash record landed in the RAM ring AND in
 *                   /var/crash/records (item 78, dump-to-flash).
 *   3. respawn   -- SIGKILL batteryd and watch init restart it
 *                   under a new pid (watchdog auto-restart).
 *
 * Same CHECK/report style as the other per-phase batteries; the
 * final "selftest: userspace ok" line is the cumulative-test hook.
 */

#include <stdint.h>

#include "crash.h"
#include "lib.h"
#include "mm/kheap.h"
#include "proc.h"
#include "signal.h"
#include "task.h"
#include "vfs.h"

#define CHECK(cond, name)                                     \
    do {                                                      \
        if (cond) {                                           \
            kprintf("usertest: %-38s ok\n", name);            \
        } else {                                              \
            kprintf("usertest: %-38s FAIL\n", name);          \
            fails++;                                          \
        }                                                     \
    } while (0)

static int fails;

/* tiny local scan (lib.h has no strstr; keep this file self-contained) */
static bool has_prefix(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (const char *p = hay; *p; p++)
        if (strncmp(p, needle, n) == 0)
            return true;
    return false;
}

static void wait_for_init(void)
{
    while (proc_pid_of_name("init") < 0)
        msleep(50);
    msleep(200);                    /* daemons settle               */
}

static void test_libc(void)
{
    int pid, code, rc;

    pid = proc_spawn("libctest",
                     (const char *const []){ "libctest", NULL },
                     NULL);
    CHECK(pid > 0, "libctest spawn");
    if (pid <= 0)
        return;

    rc = proc_kernel_wait(pid, &code, 15000u);
    CHECK(rc == pid, "libctest reaped");
    CHECK(code == 0, "libctest exit code 0");
}

static void test_crash(void)
{
    unsigned ring_before = crash_ring_count();
    int pid, code, rc;
    struct file *f;
    char buf[128];
    long r;

    pid = proc_spawn("crasher",
                     (const char *const []){ "crasher", NULL },
                     NULL);
    CHECK(pid > 0, "crasher spawn");
    if (pid <= 0)
        return;

    rc = proc_kernel_wait(pid, &code, 15000u);
    CHECK(rc == pid, "crasher reaped");

    msleep(100);                    /* VFS append settles           */

    CHECK(crash_ring_count() > ring_before, "crash ring grew");
    if (crash_ring_count() > ring_before) {
        char line[96];

        if (crash_ring_get(crash_ring_count() - 1, line,
                           sizeof(line)))
            kprintf("usertest: crash record: %s", line);
    }

    /* the flash-side proof: the record file exists and carries the
     * pid= prefix every crash_record line starts with            */
    if (vfs_open("/var/crash/records", O_RDONLY, &f) == 0) {
        r = f_read(f, buf, sizeof(buf) - 1);
        file_close(f);
        if (r > 0)
            buf[r] = 0;
        CHECK(r > 0 && has_prefix(buf, "pid="),
              "crash record in /var/crash/records");
    } else {
        CHECK(false, "crash record in /var/crash/records");
    }
}

static void test_respawn(void)
{
    int bat1 = proc_pid_of_name("batteryd");
    int bat2 = -1;

    CHECK(bat1 > 0, "batteryd running");
    if (bat1 <= 0)
        return;

    proc_do_kill(bat1, SIGKILL);

    /* init reaps the zombie and respawns; poll for the NEW pid   */
    for (int i = 0; i < 100 && bat2 < 0; i++) {
        int cur;

        msleep(50);
        cur = proc_pid_of_name("batteryd");
        if (cur > 0 && cur != bat1)
            bat2 = cur;
    }

    CHECK(bat2 > 0, "batteryd respawned by init");
    if (bat2 > 0)
        kprintf("usertest: batteryd %d -> %d (watchdog)\n",
                bat1, bat2);
}

void userspace_selftest_task(void *arg)
{
    (void)arg;

    wait_for_init();

    test_libc();
    test_crash();
    test_respawn();

    kprintf("usertest: %d failures\n", fails);
    if (!fails)
        kprintf("selftest: userspace ok\n");
    task_exit();
}
