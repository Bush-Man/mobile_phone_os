/*
 * abmgr.c - A/B update slot manager (phase 16, plan item 86).
 *
 * The table lives in one sector (a struct ab_table fits easily in
 * 512 bytes: 2 slots x 32 bytes + 16 bytes of header). All
 * mutations follow read-modify-write with the table CRC repaired
 * before the write, so a power cut mid-write leaves either the old
 * or the new table -- never a torn one (single-sector atomicity is
 * the block layer's contract).
 */

#include <stddef.h>
#include <stdint.h>

#include "abmgr.h"
#include "block.h"
#include "lib.h"
#include "syscall.h"

static struct block_device *abd;
static uint64_t table_lba;
static struct ab_table tbl;
static bool loaded;
static struct ab_stats stats;

/* ---- CRC32 (bitwise; the table is one sector, speed is moot) ---- */

static uint32_t crc32_calc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;

    while (n--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

static uint32_t table_crc(const struct ab_table *t)
{
    uint32_t crc;

    crc = crc32_calc((const uint8_t *)t, sizeof(*t) - sizeof(t->crc));
    return crc;
}

/* ---- table IO ----------------------------------------------------- */

static int table_write(void)
{
    tbl.crc = table_crc(&tbl);
    return block_write(abd, table_lba, &tbl, sizeof(tbl));
}

static bool table_valid(const struct ab_table *t)
{
    return t->magic == AB_MAGIC &&
           t->version == AB_TABLE_VERSION &&
           t->crc == table_crc(t);
}

int abmgr_attach(struct block_device *bd, uint64_t lba)
{
    if (!bd)
        return -ENODEV;

    abd = bd;
    table_lba = lba;
    loaded = false;

    if (block_read(bd, lba, &tbl, sizeof(tbl)) == 0 &&
        table_valid(&tbl)) {
        loaded = true;
        stats.active = tbl.slots[0].active ? 0
                           : (tbl.slots[1].active ? 1 : -1);
        stats.rollbacks = tbl.rollbacks;
        return 0;
    }

    /* fresh device: format an empty table (no active slot)         */
    memset(&tbl, 0, sizeof(tbl));
    tbl.magic = AB_MAGIC;
    tbl.version = AB_TABLE_VERSION;
    if (table_write())
        return -EIO;
    loaded = true;
    stats.active = -1;
    stats.rollbacks = 0;
    return 0;
}

static int table_write_checked(void)
{
    if (!loaded)
        return -ENODEV;
    return table_write() ? -EIO : 0;
}

int abmgr_active(void)
{
    return loaded ? stats.active : -1;
}

int abmgr_slot_seal(unsigned idx, uint64_t lba, uint64_t nsect,
                    uint32_t seq)
{
    struct ab_slot *s;
    uint8_t buf[BLK_SECTOR_SIZE];
    uint32_t crc = 0xffffffffu;
    uint64_t left = nsect * BLK_SECTOR_SIZE;

    if (!loaded || idx >= AB_SLOTS || !nsect)
        return -EINVAL;

    /* CRC the payload window sector by sector (streaming; the
     * window can be larger than any single buffer we keep)         */
    for (uint64_t off = 0; off < left; off += BLK_SECTOR_SIZE) {
        unsigned chunk = BLK_SECTOR_SIZE;

        if (block_read(abd, lba + off / BLK_SECTOR_SIZE,
                       buf, chunk))
            return -EIO;
        crc = crc32_calc(buf, chunk) ^ (crc << 1) ^ crc;
    }
    /* fold: the per-sector crcs combine deterministically          */
    crc ^= 0xdeadc0deu * (uint32_t)nsect;

    s = &tbl.slots[idx];
    s->crc = crc;
    s->seq = seq;
    s->lba = lba;
    s->nsect = nsect;
    s->boot_attempts = 0;
    s->confirmed = 0;
    s->active = 0;
    s->valid = 1;

    return table_write_checked();
}

int abmgr_boot_begin(void)
{
    struct ab_slot *s;

    if (!loaded || stats.active < 0)
        return -ENODEV;

    s = &tbl.slots[stats.active];
    if (s->boot_attempts < 255u)
        s->boot_attempts++;
    return table_write_checked();
}

int abmgr_confirm(void)
{
    struct ab_slot *s;

    if (!loaded || stats.active < 0)
        return -ENODEV;

    s = &tbl.slots[stats.active];
    s->confirmed = 1;
    s->boot_attempts = 0;
    s->seq++;                           /* healthy boot: version up  */
    stats.confirms++;
    return table_write_checked();
}

int abmgr_switch(void)
{
    unsigned other = 1u - (unsigned)stats.active;
    struct ab_slot *s;

    if (!loaded || stats.active < 0)
        return -ENODEV;
    if (!tbl.slots[other].valid)
        return -EINVAL;                 /* nowhere to switch to      */

    tbl.slots[stats.active].active = 0;
    s = &tbl.slots[other];
    s->active = 1;
    s->boot_attempts = 0;
    stats.active = (int)other;
    stats.switches++;
    return table_write_checked();
}

int abmgr_evaluate(void)
{
    struct ab_slot *s;

    if (!loaded || stats.active < 0)
        return -ENODEV;

    s = &tbl.slots[stats.active];
    if (s->confirmed || s->boot_attempts < AB_MAX_ATTEMPTS)
        return 0;                       /* healthy, or still trying  */

    /* unconfirmed slot exhausted its attempts: roll back           */
    stats.rollbacks++;
    tbl.rollbacks = stats.rollbacks;
    (void)table_write_checked();
    return abmgr_switch();
}

int abmgr_table_get(struct ab_table *out)
{
    if (!loaded || !out)
        return -ENODEV;
    memcpy(out, &tbl, sizeof(*out));
    return 0;
}

int abmgr_table_put(const struct ab_table *in)
{
    if (!loaded || !in || !table_valid(in))
        return -EINVAL;
    memcpy(&tbl, in, sizeof(tbl));
    return table_write_checked();
}

struct block_device *abmgr_device(void)
{
    return abd;
}

uint64_t abmgr_table_lba(void)
{
    return table_lba;
}
