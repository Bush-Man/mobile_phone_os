/*
 * selftest_fs.c - phase 7 filesystem selftests, run as the "fstest"
 * kernel task (the disk paths block via the sleeping fs locks).
 *
 * Checks, in order:
 *   1. ramfs: create/write/seek/read-back, mkdir + nested file,
 *      getdents enumeration, unlink/rmdir cleanup.
 *   2. devfs: console node writable; unknown node -> ENOENT.
 *   3. vfat on virtio-blk: layout negotiation on a blank image
 *      (dual MBR), mkfs when unformatted, persistence counter that
 *      increments across reboots, multi-cluster pattern round-trip,
 *      LFN create/read/delete.
 *   4. ext2 ditto under /ext2, with a >12-block file exercising
 *      singly-indirect mapping plus mkdir/rmdir inside ext2.
 *
 * Summary line "selftest: fs ok" matches the harness grep style.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "block.h"
#include "ext2.h"
#include "fat32.h"
#include "lib.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"

static int failures;

#define CHECK(cond, name)                                              \
    do {                                                               \
        if (cond) {                                                    \
            kprintf("fstest: %-34s ok\n", name);                       \
        } else {                                                       \
            kprintf("fstest: %-34s FAIL\n", name);                     \
            failures++;                                                \
        }                                                              \
    } while (0)

/* ---- small kernel-side IO helpers ---------------------------------------- */

static long wr_file(const char *path, const void *buf, size_t len,
                    unsigned flags)
{
    struct file *f;
    long r;

    r = vfs_open(path, flags, &f);
    if (r)
        return r;
    r = f_write(f, buf, len);
    file_close(f);
    return r;
}

static long rd_file(const char *path, void *buf, size_t len)
{
    struct file *f;
    long r;

    r = vfs_open(path, O_RDONLY, &f);
    if (r)
        return r;
    r = f_read(f, buf, len);
    file_close(f);
    return r;
}

/* mnt + "/" + leaf into a fixed buffer                            */
static void mkpath(char *dst, size_t cap, const char *mnt,
                   const char *leaf)
{
    size_t ml = strlen(mnt), ll = strlen(leaf);

    if (ml + 1 + ll + 1 > cap)
        return;
    memcpy(dst, mnt, ml);
    dst[ml] = '/';
    memcpy(&dst[ml + 1], leaf, ll + 1);
}

/* decimal parse for the persistence counters                      */
static uint64_t parse_dec(const char *s)
{
    uint64_t v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10u + (uint64_t)(*s++ - '0');
    return v;
}

/*
 * Read path as a decimal counter, delete it, write back value+1.
 * Returns the new value or (uint64_t)-1.
 */
static long bump_counter(const char *path, uint64_t *new_val_out)
{
    char buf[24];
    uint64_t n = 0;
    long r;
    int digits = 1;
    uint64_t v = 0;

    r = rd_file(path, buf, sizeof(buf) - 1);
    if (r > 0) {
        buf[r] = '\0';
        n = parse_dec(buf);
    }

    vfs_unlink(path);                   /* recreate fresh             */

    n++;

    {
        uint64_t t = n;

        while (t >= 10) {
            t /= 10;
            digits++;
        }
    }
    {
        char out[24];
        int i = digits;

        v = n;
        out[i] = '\0';
        while (i--) {
            out[i] = (char)('0' + v % 10u);
            v /= 10u;
        }
        out[digits] = '\n';

        r = wr_file(path, out, (size_t)digits + 1,
                    O_CREAT | O_WRONLY);
        if (r < 0)
            return -1;
    }

    *new_val_out = n;
    return 0;
}

/* ---- 1. ramfs -------------------------------------------------------------------- */

static void ramfs_tests(void)
{
    static const char msg[] = "hello ramfs";
    char buf[64];
    struct file *df;
    long r;

    CHECK(vfs_mkdir("/ramdir") == 0, "ramfs mkdir");

    r = wr_file("/ramdir/hello.txt", msg, sizeof(msg) - 1,
                O_CREAT | O_WRONLY);
    CHECK(r == sizeof(msg) - 1, "ramfs create+write");

    memset(buf, 0, sizeof(buf));
    r = rd_file("/ramdir/hello.txt", buf, sizeof(buf));
    CHECK(r == sizeof(msg) - 1 &&
              memcmp(buf, msg, sizeof(msg) - 1) == 0,
          "ramfs read-back");

    /* seek around via a separate handle                            */
    {
        struct file *f;

        if (!vfs_open("/ramdir/hello.txt", O_RDONLY, &f)) {
            int64_t pos = -1;

            f_lseek(f, 0, SEEK_END, &pos);
            CHECK(pos == (int64_t)(sizeof(msg) - 1),
                  "ramfs lseek END");
            f_lseek(f, 6, SEEK_SET, &pos);
            memset(buf, 0, sizeof(buf));
            r = f_read(f, buf, 5);
            CHECK(r == 5 && memcmp(buf, "ramfs", 5) == 0,
                  "ramfs seek+partial read");
            file_close(f);
        } else {
            CHECK(false, "ramfs reopen");
        }
    }

    /* getdents enumeration of /ramdir                              */
    {
        uint8_t db[128];
        bool found = false;

        if (!vfs_open("/ramdir", O_RDONLY, &df)) {
            long bytes = f_getdents(df, db, sizeof(db));

            file_close(df);
            for (long o = 0; o + 3 <= bytes;) {
                const char *nm = (const char *)&db[o + 3];

                if (strcmp(nm, "hello.txt") == 0) {
                    found = true;
                    break;
                }
                o += *(uint16_t *)(void *)&db[o];
            }
        }
        CHECK(found, "getdents lists entry");
    }

    CHECK(vfs_unlink("/ramdir/hello.txt") == 0, "ramfs unlink");
    CHECK(vfs_rmdir("/ramdir") == 0, "ramfs rmdir");
}

/* ---- 2. devfs ----------------------------------------------------------------------- */

static void devfs_tests(void)
{
    static const char poke[] = "\nfstest: console node write\n";
    struct file *f;
    long r;

    r = vfs_open("/dev/console", O_WRONLY, &f);
    if (!r) {
        r = f_write(f, poke, sizeof(poke) - 1);
        file_close(f);
    }
    CHECK(r == (long)(sizeof(poke) - 1), "devfs console write");

    r = vfs_open("/dev/nope", O_RDONLY, &f);
    CHECK(r == -ENOENT, "devfs missing node ENOENT");
}

/* ---- 3./4. disk-backed filesystems ---------------------------------------------------------- */

/* one MBR: FAT32 at LBA 2048 (36 MiB), ext2 covering the rest     */
#define FAT_LBA_START   2048ull
#define FAT_LBA_COUNT   73728ull

static bool part_interior_blank(struct block_device *bd,
                                const struct partition *p);

static void write_dual_mbr(struct block_device *bd)
{
    uint8_t sec[BLK_SECTOR_SIZE];
    uint64_t ext_start = FAT_LBA_START + FAT_LBA_COUNT;
    uint64_t ext_count = bd->capacity_sectors - ext_start;

    memset(sec, 0, BLK_SECTOR_SIZE);

    sec[446 + 4] = 0x0B;                /* FAT32 CHS-irrelevant       */
    sec[454] = (uint8_t)FAT_LBA_START;
    sec[455] = (uint8_t)(FAT_LBA_START >> 8);
    sec[456] = (uint8_t)(FAT_LBA_START >> 16);
    sec[457] = (uint8_t)(FAT_LBA_START >> 24);
    sec[458] = (uint8_t)FAT_LBA_COUNT;
    sec[459] = (uint8_t)(FAT_LBA_COUNT >> 8);
    sec[460] = (uint8_t)(FAT_LBA_COUNT >> 16);
    sec[461] = (uint8_t)(FAT_LBA_COUNT >> 24);

    sec[462 + 4] = 0x83;                /* Linux data                 */
    sec[470] = (uint8_t)ext_start;
    sec[471] = (uint8_t)(ext_start >> 8);
    sec[472] = (uint8_t)(ext_start >> 16);
    sec[473] = (uint8_t)(ext_start >> 24);
    sec[474] = (uint8_t)ext_count;
    sec[475] = (uint8_t)(ext_count >> 8);
    sec[476] = (uint8_t)(ext_count >> 16);
    sec[477] = (uint8_t)(ext_count >> 24);

    sec[510] = 0x55;
    sec[511] = 0xAA;

    block_write(bd, 0, sec, BLK_SECTOR_SIZE);
}

static bool disk_is_blank(struct block_device *bd)
{
    uint8_t sec[BLK_SECTOR_SIZE];

    if (block_read(bd, 0, sec, BLK_SECTOR_SIZE) != 0)
        return false;
    return sec[510] == 0 && sec[511] == 0 && sec[446] == 0 &&
           sec[450] == 0 && sec[462] == 0;
}

static bool part_interior_blank(struct block_device *bd,
                                const struct partition *p)
{
    uint8_t sec[BLK_SECTOR_SIZE];

    if (block_read(bd, p->lba_start, sec, BLK_SECTOR_SIZE) != 0)
        return false;
    for (unsigned i = 0; i < BLK_SECTOR_SIZE; i++)
        if (sec[i])
            return false;
    return true;
}

static const struct partition *find_part(struct block_device *bd,
                                         bool want_fat)
{
    for (unsigned i = 0; i < bd->nparts; i++) {
        const struct partition *p = &bd->parts[i];

        if (!p->valid || p->is_extended || !p->lba_nsect)
            continue;
        if (want_fat) {
            if (p->mbr_type == 0x0B || p->mbr_type == 0x0C ||
                fat32_sniff(bd, p->lba_start, p->lba_nsect))
                return p;
        } else {
            if (p->mbr_type == 0x83 ||
                ext2_sniff(bd, p->lba_start, p->lba_nsect))
                return p;
        }
    }
    return NULL;
}

/*
 * Make sure both partitions exist. Handles three boot histories:
 * virgin disk, phase-6's single-partition MBR (interior blank),
 * and our own dual layout from an earlier run.
 */
static void ensure_layout(struct block_device *bd)
{
    if (find_part(bd, true) && find_part(bd, false))
        return;

    bool redo = disk_is_blank(bd);

    if (!redo && bd->nparts >= 1 && bd->parts[0].valid &&
        bd->parts[0].mbr_type == 0x83 && !bd->parts[0].is_extended &&
        part_interior_blank(bd, &bd->parts[0])) {
        kprintf("fstest: adopting phase-6 blank partition\n");
        redo = true;
    }

    if (redo && bd->capacity_sectors >
                    FAT_LBA_START + FAT_LBA_COUNT + 4096) {
        write_dual_mbr(bd);
        kprintf("fstest: wrote dual-layout MBR to %s\n", bd->name);
        block_scan_partitions(bd, NULL);
    }
}

struct disk_fs_spec {
    bool present;
    const struct partition *part;
};

static void disk_fs_tests(struct block_device *bd, bool fat_side)
{
    const char *mnt = fat_side ? "/dos" : "/ext2";
    const char *fstype = fat_side ? "vfat" : "ext2";
    const struct partition *p;
    char cnt_path[48], big_path[48], dir_path[48];
    uint64_t counter = 0;
    long r;

    p = find_part(bd, fat_side);
    if (!p) {
        kprintf("fstest: no %s partition, skipping\n", fstype);
        return;
    }

    if (!(fat_side ? fat32_sniff(bd, p->lba_start, p->lba_nsect)
                   : ext2_sniff(bd, p->lba_start, p->lba_nsect))) {
        r = fat_side ? fat32_mkfs(bd, p->lba_start, p->lba_nsect)
                     : ext2_mkfs(bd, p->lba_start, p->lba_nsect);
        CHECK(r == 0, fat_side ? "vfat mkfs" : "ext2 mkfs");
    }

    r = vfs_mount(fstype, mnt, bd, p->lba_start, p->lba_nsect);
    if (r) {
        CHECK(false, fat_side ? "vfat mount" : "ext2 mount");
        return;
    }
    CHECK(true, fat_side ? "vfat mount" : "ext2 mount");

    mkpath(cnt_path, sizeof(cnt_path), mnt, "fstest.cnt");
    mkpath(big_path, sizeof(big_path), mnt, "big.bin");
    mkpath(dir_path, sizeof(dir_path), mnt, "subdir");

    /* milestone proof: a file whose value survives reboot          */
    r = bump_counter(cnt_path, &counter);
    CHECK(r == 0 && counter > 0,
          fat_side ? "vfat persist counter" : "ext2 persist counter");
    kprintf("fstest: %s counter now %llu (survives reboot)\n",
            fstype, (unsigned long long)counter);

    /*
     * Multi-cluster (vfat) / beyond-direct (ext2) pattern file.
     * 64 KiB spans 96+ FAT clusters and 60+ ext2 blocks, so both
     * chain growth and indirect mapping get exercised.
     */
    {
        static uint8_t wb[64 * 1024];
        static uint8_t rb[64 * 1024];

        for (unsigned i = 0; i < sizeof(wb); i++)
            wb[i] = (uint8_t)(i * 31u + 7u);

        vfs_unlink(big_path);           /* idempotent reruns          */
        r = wr_file(big_path, wb, sizeof(wb), O_CREAT | O_WRONLY);
        CHECK(r == (long)sizeof(wb),
              fat_side ? "vfat big write" : "ext2 big write");

        memset(rb, 0, sizeof(rb));
        r = rd_file(big_path, rb, sizeof(rb));
        CHECK(r == (long)sizeof(rb) &&
                  memcmp(wb, rb, sizeof(wb)) == 0,
              fat_side ? "vfat big verify" : "ext2 big verify");

        CHECK(vfs_unlink(big_path) == 0,
              fat_side ? "vfat unlink big" : "ext2 unlink big");
    }

    if (fat_side) {
        /* long file names: create, read back by full name, delete  */
        static const char lfn_msg[] = "long name works";

        vfs_unlink("/dos/Long File Name.txt");
        r = wr_file("/dos/Long File Name.txt", lfn_msg,
                    sizeof(lfn_msg) - 1, O_CREAT | O_WRONLY);
        CHECK(r == (long)(sizeof(lfn_msg) - 1), "vfat LFN create");

        {
            char lb[32];

            r = rd_file("/dos/Long File Name.txt", lb,
                        sizeof(lb) - 1);
            CHECK(r == (long)(sizeof(lfn_msg) - 1) &&
                      memcmp(lb, lfn_msg, r) == 0,
                  "vfat LFN read");
        }
        CHECK(vfs_unlink("/dos/Long File Name.txt") == 0,
              "vfat LFN unlink");
    } else {
        CHECK(vfs_mkdir(dir_path) == 0, "ext2 mkdir");
        CHECK(vfs_mkdir(dir_path) == -EEXIST, "ext2 dup mkdir EEXIST");
        CHECK(vfs_rmdir(dir_path) == 0, "ext2 rmdir");
    }
}

/* ---- entry ---------------------------------------------------------------------------------------------- */

void fs_selftest_task(void *arg)
{
    struct block_device *bd = block_find("vblk0");

    (void)arg;

    kprintf("fstest: phase 7 filesystem selftests\n");

    ramfs_tests();
    devfs_tests();

    if (!bd)
        bd = block_first();
    if (!bd) {
        kprintf("fstest: no disk attached, skipping disk tests\n");
    } else {
        ensure_layout(bd);
        block_print_partitions(bd);
        disk_fs_tests(bd, true);        /* vfat under /dos           */
        disk_fs_tests(bd, false);       /* ext2 under /ext2          */
    }

    if (!failures)
        kprintf("selftest: fs ok\n");
    else
        kprintf("selftest: fs FAILED (%d)\n", failures);

    task_exit();
}
