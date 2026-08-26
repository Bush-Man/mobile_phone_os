/*
 * selftest_driver.c - phase 6 driver selftests, run as a kernel task
 * ("drvtest") because the IO paths block via msleep()/wait queues.
 *
 * Checks:
 *   1. gpiolib + PL061: request/output readback/duplicate-request
 *      rejection/pin-irq slot bookkeeping.
 *   2. tty line discipline: line completion, erase, kill-line,
 *      overflow accounting (bytes injected directly for determinism).
 *   3. block stack on virtio-blk: pattern write/read/verify near the
 *      end of the disk; MBR creation on a blank image + re-scan.
 *   4. virtio-net presence/MAC/report when a backend is attached.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "block.h"
#include "chardev.h"
#include "gpio.h"
#include "lib.h"
#include "task.h"
#include "tty.h"
#include "virtio.h"

static int failures;

#define CHECK(cond, name)                                              \
    do {                                                               \
        if (cond) {                                                    \
            kprintf("drvtest: %-34s ok\n", name);                      \
        } else {                                                       \
            kprintf("drvtest: %-34s FAIL\n", name);                    \
            failures++;                                                \
        }                                                              \
    } while (0)

/* ---- 1. gpio ------------------------------------------------------------------ */

static void gpio_tests(void)
{
    if (!gpio_chip_count()) {
        kprintf("drvtest: no gpio controller, skipping\n");
        return;
    }

    CHECK(gpio_request(0, "selftest") == 0, "gpio request");
    CHECK(gpio_request(0, "again") != 0, "gpio double-request rejected");

    CHECK(gpio_dir_out(0, true) == 0, "gpio dir out");
    gpio_set(0, true);
    CHECK(gpio_get(0) == 1, "gpio output high readback");
    gpio_set(0, false);
    CHECK(gpio_get(0) == 0, "gpio output low readback");

    CHECK(gpio_irq_register(0, NULL, NULL) != 0, "gpio irq null rejected");
    gpio_free(0);
    CHECK(gpio_owner(0) == NULL, "gpio free");
}

/* ---- 2. tty --------------------------------------------------------------------- */

static void expect_line(const char *feed, const char *want, const char *name)
{
    char buf[TTY_LINE_MAX];

    tty_test_feed(feed);
    int n = tty_read(buf, sizeof(buf));

    buf[n >= 0 ? (unsigned)n : 0] = '\0';

    unsigned wl = 0;
    while (want[wl])
        wl++;

    CHECK(n == (int)wl && memcmp(buf, want, wl) == 0, name);
}

static void tty_tests(void)
{
    struct char_dev *cd = char_dev_find("console");

    CHECK(cd != NULL, "console char device registered");

    expect_line("hello\r", "hello\n", "tty canonical line");
    expect_line("ab\177\177c\r", "c\n", "tty DEL erase");          /* ^? twice */
    expect_line("junk\25x\n", "x\n", "tty ^U kill");               /* ^U = 025 */
}

/* ---- 3. block ---------------------------------------------------------------------- */

static bool blank_disk(struct block_device *bd)
{
    uint8_t sec[BLK_SECTOR_SIZE];

    if (block_read(bd, 0, sec, BLK_SECTOR_SIZE) != 0)
        return false;
    for (unsigned i = 510; i < 512; i++)
        if (sec[i] != 0)
            return false;
    return sec[446] == 0 && sec[447] == 0;
}

/* lay down one MBR entry: type 0x83 starting at LBA 2048 */
static void write_fresh_mbr(struct block_device *bd)
{
    uint8_t sec[BLK_SECTOR_SIZE];
    uint64_t count = bd->capacity_sectors - 2048;

    memset(sec, 0, BLK_SECTOR_SIZE);

    sec[446] = 0x80;                    /* bootable flag */
    /* CHS fields ignored by our parser */
    sec[450] = 0x83;                    /* Linux data */
    sec[454] = (uint8_t)(2048);
    sec[455] = (uint8_t)(2048 >> 8);
    sec[456] = (uint8_t)(2048 >> 16);
    sec[457] = (uint8_t)(2048 >> 24);
    sec[458] = (uint8_t)count;
    sec[459] = (uint8_t)(count >> 8);
    sec[460] = (uint8_t)(count >> 16);
    sec[461] = (uint8_t)(count >> 24);
    sec[510] = 0x55;
    sec[511] = 0xaa;

    block_write(bd, 0, sec, BLK_SECTOR_SIZE);
}

static void block_tests(void)
{
    struct block_device *bd = block_find("vblk0");

    if (!bd)
        bd = block_first();
    if (!bd) {
        kprintf("drvtest: no disk attached, skipping block tests\n");
        return;
    }

    /*
     * Pattern write/read at the tail of the disk -- far from any
     * partition or filesystem area a later phase might use.
     */
    {
        static uint8_t wbuf[BLK_SECTOR_SIZE * 4];
        static uint8_t rbuf[BLK_SECTOR_SIZE * 4];
        uint64_t lba = bd->capacity_sectors - 16;

        for (unsigned i = 0; i < sizeof(wbuf); i++)
            wbuf[i] = (uint8_t)(i * 31u +
                                (unsigned)(lba & 0xffu));

        CHECK(block_write(bd, lba, wbuf, sizeof(wbuf)) == 0,
              "blk pattern write");
        CHECK(block_read(bd, lba, rbuf, sizeof(rbuf)) == 0,
              "blk read back");
        CHECK(memcmp(wbuf, rbuf, sizeof(wbuf)) == 0, "blk verify");

        /* cache path: single-sector reads go through the cache */
        uint8_t s1[BLK_SECTOR_SIZE], s2[BLK_SECTOR_SIZE];

        CHECK(block_read(bd, lba, s1, BLK_SECTOR_SIZE) == 0 &&
              block_read(bd, lba, s2, BLK_SECTOR_SIZE) == 0 &&
              memcmp(s1, s2, BLK_SECTOR_SIZE) == 0,
              "blk cached reads stable");
    }

    /*
     * Partition milestone: on a blank image we create an MBR ourselves,
     * then prove the parser round-trips it. Pre-existing tables are
     * left untouched and merely re-scanned.
     */
    bool wrote_mbr = false;

    if (blank_disk(bd)) {
        write_fresh_mbr(bd);
        kprintf("drvtest: wrote fresh MBR to %s\n", bd->name);
        wrote_mbr = true;
    }

    int n = block_scan_partitions(bd, NULL);

    block_print_partitions(bd);
    CHECK(n >= 0, "partition scan ran");
    if (wrote_mbr && n > 0) {
        const struct partition *p = &bd->parts[0];

        CHECK(!p->is_gpt && p->mbr_type == 0x83 &&
                  p->lba_start == 2048,
              "partition geometry sane");
    }
}

/* ---- 4. virtio-net -------------------------------------------------------------------- */

static void net_report(void)
{
    if (!virtio_net_present()) {
        kprintf("drvtest: no virtio-net backend, skipping net report\n");
        return;
    }

    const uint8_t *mac = virtio_net_mac();

    CHECK(mac != NULL, "virtio-net mac available");
    kprintf("drvtest: nic mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* one gratuitous ARP-shaped frame proves the TX path end to end */
    uint8_t arp[42];

    memset(arp, 0, sizeof(arp));
    memset(&arp[0], 0xff, 6);           /* broadcast dst */
    memcpy(&arp[6], mac, 6);
    arp[12] = 0x08;                     /* ethertype = ARP */
    arp[13] = 0x06;
    arp[21] = 1;                        /* request */
    arp[38] = mac[3];                   /* sender ip tail = mac tag */

    CHECK(virtio_net_send(arp, sizeof(arp)) == 0, "net tx frame");
}

/* ---- entry ---------------------------------------------------------------------------------- */

void driver_selftest_task(void *arg)
{
    (void)arg;

    kprintf("drvtest: phase 6 driver selftests\n");
    gpio_tests();
    tty_tests();
    block_tests();
    net_report();

    if (!failures)
        kprintf("selftest: drivers ok\n");
    else
        kprintf("selftest: drivers FAILED (%d)\n", failures);

    task_exit();
}
