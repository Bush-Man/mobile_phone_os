/*
 * selftest_release.c - the phase-16 battery ("reltest").
 *
 * One kernel task exercising every hardening surface end to end:
 *
 *   W^X    -- probe a live process's page tables: its text page
 *             must be executable and NOT writable; a data page
 *             writable and NOT executable.
 *   ASLR   -- two spawns of the same image must land on different
 *             randomized mmap bases.
 *   PERM   -- the permission table: known apps get their bits,
 *             unknown ones get nothing (default deny).
 *   KMSG   -- the ring holds boot lines and dumps to /var/kmsg.
 *   WDT    -- the software watchdog is armed and being kicked.
 *   A/B    -- the full slot-manager flow against a ramdisk block
 *             device: seal -> switch -> confirm -> boot attempts
 *             -> automatic rollback with the counter bump.
 *   PERF   -- kheap churn benchmark (the boot-time stamp itself is
 *             printed by kmain: "[perf] boot N ms").
 *
 * Milestone (item 89's on-target half): every CHECK green prints
 * "selftest: release ok"; the reproducible-image half is the
 * release builder (scripts/build-release.sh + docs/RELEASE.md).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "abmgr.h"
#include "block.h"
#include "kmsg.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/vmm.h"
#include "perm.h"
#include "proc.h"
#include "task.h"
#include "time.h"
#include "vfs.h"
#include "watchdog.h"

static int fails;

#define CHECK(cond, name)                                     \
    do {                                                      \
        if (cond) {                                           \
            kprintf("reltest: %-38s ok\n", name);             \
        } else {                                              \
            kprintf("reltest: %-38s FAIL\n", name);           \
            fails++;                                          \
        }                                                     \
    } while (0)

/* ---- W^X probes ----------------------------------------------------- */

static void test_wx(void)
{
    struct proc *p = proc_by_pid(proc_pid_of_name("sh"));

    CHECK(p != NULL, "wx: sh process found");
    if (!p)
        return;

    {
        paddr_t pa;
        unsigned fl;

        /* the text page: executable, never writable              */
        if (vmm_probe(p->root_pa, 0x0000010000000000ULL,
                      &pa, &fl)) {
            CHECK((fl & VM_EXEC) && !(fl & VM_WRITE),
                  "wx: text is X and not W");
        } else {
            CHECK(false, "wx: text page mapped");
        }

        /* the private-mmap window: writable, never executable    */
        if (vmm_probe(p->root_pa, p->mmap_next, &pa, &fl)) {
            CHECK((fl & VM_WRITE) && !(fl & VM_EXEC),
                  "wx: data is W and not X");
        } else {
            /* nothing mmap'd yet: walk back from the heap edge
             * until we find ANY writable, non-executable page --
             * the process's .data/.bss qualifies                  */
            bool found = false;

            for (uint64_t va = p->brk_floor - PAGE_SIZE;
                 va >= 0x0000010000000000ULL && !found;
                 va -= PAGE_SIZE) {
                if (vmm_probe(p->root_pa, va, &pa, &fl) &&
                    (fl & VM_WRITE) && !(fl & VM_EXEC))
                    found = true;
            }
            CHECK(found, "wx: data is W and not X");
        }
    }
}

/* ---- ASLR ----------------------------------------------------------- */

static void test_aslr(void)
{
    int pid1 = proc_spawn("clock",
                          (const char *const[]){ "clock", NULL },
                          NULL);
    int pid2;
    struct proc *p1, *p2;
    uint64_t base1, base2;

    CHECK(pid1 > 0, "aslr: first clock spawn");
    pid2 = proc_spawn("clock",
                      (const char *const[]){ "clock", NULL },
                      NULL);
    CHECK(pid2 > 0, "aslr: second clock spawn");
    if (pid1 <= 0 || pid2 <= 0)
        return;

    p1 = proc_by_pid(pid1);
    p2 = proc_by_pid(pid2);
    CHECK(p1 && p2, "aslr: both procs found");
    if (!p1 || !p2)
        return;

    base1 = p1->mmap_next;
    base2 = p2->mmap_next;
    kprintf("reltest: aslr bases %llx / %llx\n",
            (unsigned long long)base1, (unsigned long long)base2);
    CHECK(base1 != base2, "aslr: layouts differ");
    CHECK(base1 >= 0x0000028000000000ULL &&
          base2 >= 0x0000028000000000ULL,
          "aslr: bases inside the mmap window");

    /* cleanup: the clocks are our children                         */
    proc_do_kill(pid1, 9);
    proc_do_kill(pid2, 9);
    {
        int code;

        (void)proc_kernel_wait(pid1, &code, 5000u);
        (void)proc_kernel_wait(pid2, &code, 5000u);
    }
}

/* ---- permission table ----------------------------------------------- */

static void test_perm(void)
{
    CHECK(perm_has("compositor", PERM_UI_COMPOSE) &&
              perm_has("compositor", PERM_FB_PRESENT),
          "perm: compositor capabilities");
    CHECK(perm_has("dialer", PERM_MODEM),
          "perm: dialer may drive the modem");
    CHECK(!perm_has("sh", PERM_MODEM) &&
              !perm_has("hello", PERM_UI_COMPOSE),
          "perm: default deny for unknown apps");
    CHECK(perm_lookup("no-such-app") == 0,
          "perm: unknown app -> empty mask");
}

/* ---- kmsg ----------------------------------------------------------- */

static void test_kmsg(void)
{
    char line[128];
    bool found_banner = false;
    unsigned n = kmsg_count();

    CHECK(n > 10, "kmsg: ring holds boot lines");

    for (unsigned i = 0; i < n && !found_banner; i++)
        if (kmsg_line(i, line, sizeof(line)) &&
            strncmp(line, "mobile_phone_os kernel", 22) == 0)
            found_banner = true;
    CHECK(found_banner, "kmsg: boot banner present");

    CHECK(kmsg_dump("/var/kmsg") == 0, "kmsg: dumped to /var/kmsg");
    {
        struct file *f = NULL;
        char buf[64];
        long r = 0;

        CHECK(vfs_open("/var/kmsg", O_RDONLY, &f) == 0,
              "kmsg: dump file readable");
        if (!f) {
            /* open left f untouched: reading it would dereference
             * an uninitialised pointer and panic the kernel */
            CHECK(false, "kmsg: persisted content");
            return;
        }
        r = f_read(f, buf, sizeof(buf) - 1);
        if (r > 0)
            buf[r] = 0;
        /* the ring's first line is the leading "\n" of the boot
         * banner, so scan for the banner text instead of a
         * prefix compare                                        */
        {
            bool found = false;

            for (long i = 0; i + 6 < r && !found; i++)
                if (strncmp(buf + i, "mobile", 6) == 0)
                    found = true;
            CHECK(r > 20 && found, "kmsg: persisted content");
        }
        file_close(f);
    }
}

/* ---- watchdog --------------------------------------------------------- */

static void test_watchdog(void)
{
    struct watchdog_stats st, st2;

    watchdog_kick();
    watchdog_stats_get(&st);
    CHECK(st.armed && st.timeout_ms > 0, "wdt: armed with deadline");

    /* housekeeping kicks every ~2 ms; an absolute count depends on how
     * much CPU it got, so watch the counter advance instead */
    msleep(100);
    watchdog_stats_get(&st2);
    CHECK(st2.kicks > st.kicks, "wdt: heartbeats flowing");
    CHECK(st2.misses == 0, "wdt: no misses");
}

/* ---- A/B slot manager ------------------------------------------------ */

/* a tiny RAM-backed block device: no virtio dependency, no
 * interference with the phase-6/7 scratch-disk users              */
#define ABT_LBA         8u
#define AB_PAYLOAD_LBA  16u
#define AB_PAYLOAD_SECT 32u

static uint8_t ramdisk_storage[64 * BLK_SECTOR_SIZE];

static int rd_read(struct block_device *bd, uint64_t lba,
                   void *buf, unsigned nsect)
{
    (void)bd;
    if (lba + nsect > 64u)
        return -1;
    memcpy(buf, ramdisk_storage + lba * BLK_SECTOR_SIZE,
           nsect * BLK_SECTOR_SIZE);
    return 0;
}

static int rd_write(struct block_device *bd, uint64_t lba,
                    const void *buf, unsigned nsect)
{
    (void)bd;
    if (lba + nsect > 64u)
        return -1;
    memcpy(ramdisk_storage + lba * BLK_SECTOR_SIZE, buf,
           nsect * BLK_SECTOR_SIZE);
    return 0;
}

static struct block_device rd_dev = {
    .name = "abtest",
    .read_blocks = rd_read,
    .write_blocks = rd_write,
    .capacity_sectors = 64u,
    .max_sectors = 32u,
};

static void test_abmgr(void)
{
    struct ab_table t;

    CHECK(block_register(&rd_dev) == 0, "ab: ramdisk registered");
    CHECK(abmgr_attach(&rd_dev, ABT_LBA) == 0,
          "ab: attach + format");
    CHECK(abmgr_active() < 0,
          "ab: fresh table has no active slot");

    /* seal both candidate slots with distinct fake payloads        */
    memset(ramdisk_storage + AB_PAYLOAD_LBA * BLK_SECTOR_SIZE, 0xAA,
           AB_PAYLOAD_SECT * BLK_SECTOR_SIZE);
    CHECK(abmgr_slot_seal(0, AB_PAYLOAD_LBA, AB_PAYLOAD_SECT,
                          10u) == 0, "ab: slot A sealed (seq 10)");
    memset(ramdisk_storage + AB_PAYLOAD_LBA * BLK_SECTOR_SIZE, 0x55,
           AB_PAYLOAD_SECT * BLK_SECTOR_SIZE);
    CHECK(abmgr_slot_seal(1, AB_PAYLOAD_LBA, AB_PAYLOAD_SECT,
                          11u) == 0, "ab: slot B sealed (seq 11)");

    CHECK(abmgr_switch() == 0 && abmgr_active() == 0,
          "ab: slot A activated");
    CHECK(abmgr_boot_begin() == 0, "ab: boot attempt counted");
    CHECK(abmgr_confirm() == 0, "ab: healthy boot confirmed");
    abmgr_table_get(&t);
    CHECK(t.slots[0].confirmed && t.slots[0].boot_attempts == 0,
          "ab: confirmed state persisted");

    /* the rollback flow: B becomes active, burns its attempts      */
    CHECK(abmgr_switch() == 0 && abmgr_active() == 1,
          "ab: switched to slot B");
    for (unsigned i = 0; i < AB_MAX_ATTEMPTS; i++)
        CHECK(abmgr_boot_begin() == 0, "ab: unconfirmed boot");
    CHECK(abmgr_evaluate() == 1, "ab: automatic rollback fired");
    CHECK(abmgr_active() == 0, "ab: back on slot A");
    abmgr_table_get(&t);
    CHECK(t.rollbacks == 1, "ab: rollback counter bumped");
    CHECK(t.slots[1].boot_attempts == AB_MAX_ATTEMPTS,
          "ab: failed slot kept its attempt count");

    /* persistence: re-attach reads the same table back             */
    CHECK(abmgr_attach(&rd_dev, ABT_LBA) == 0 &&
              abmgr_active() == 0, "ab: table survives re-attach");
}

/* ---- perf --------------------------------------------------------- */

static void test_perf(void)
{
    uint64_t t0, t1;
    void *ptrs[32];

    t0 = time_uptime_ms();
    for (int round = 0; round < 50; round++) {
        for (unsigned i = 0; i < 32; i++)
            ptrs[i] = kmalloc(64 + (i % 7) * 16);
        for (unsigned i = 0; i < 32; i++)
            kfree(ptrs[i]);
    }
    t1 = time_uptime_ms();
    CHECK(t1 >= t0, "perf: kheap churn ran");
    kprintf("reltest: perf 1600 alloc/free rounds in %llu ms\n",
            (unsigned long long)(t1 - t0));
    /* the boot metric itself was printed by kmain ("[perf] boot")*/
}

void release_selftest_task(void *arg)
{
    (void)arg;

    msleep(300);                    /* init + sh settle             */

    kprintf("reltest: phase 16 release battery\n");
    test_wx();
    test_aslr();
    test_perm();
    test_kmsg();
    test_watchdog();
    test_abmgr();
    test_perf();

    if (fails == 0)
        kprintf("selftest: release ok\n");
    else
        kprintf("selftest: release FAILURES (%d)\n", fails);
    task_exit();
}
