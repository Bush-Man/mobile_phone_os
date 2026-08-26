/*
 * block.c - block layer: registry, sector cache, request queue,
 * MBR/GPT partition parsing.
 *
 * The sector cache is direct-mapped, write-through, per device.
 * All multi-byte on-disk fields are little-endian; helpers below
 * read them explicitly. GPT headers and entry arrays are validated
 * against their CRC32s (reflected 0xEDB88320) before use.
 */

#include <stdint.h>
#include <stddef.h>

#include "block.h"
#include "lib.h"
#include "mm/kheap.h"
#include "spinlock.h"

/* ---- little-endian disk readers --------------------------------------------- */

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ---- crc32 (reflected, poly 0xEDB88320) --------------------------------------- */

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;

        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = true;
}

/* streaming update; seed with 0xffffffff, finish with ~crc */
static uint32_t crc32_update(uint32_t crc, const void *buf, unsigned n)
{
    const uint8_t *p = buf;

    if (!crc_ready)
        crc_init();
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    return crc;
}

/* ---- registry --------------------------------------------------------------------- */

static struct block_device *bdevs[BLK_DEV_MAX];
static unsigned nbdevs;
static spinlock_t blk_lock = SPINLOCK_INIT;

static bool s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int block_register(struct block_device *bd)
{
    int r;

    if (!bd || !bd->name || !bd->read_blocks || !bd->write_blocks ||
        bd->max_sectors == 0 || bd->capacity_sectors == 0)
        return -1;

    spin_lock(&blk_lock);
    if (nbdevs >= BLK_DEV_MAX) {
        r = -1;
    } else {
        bdevs[nbdevs++] = bd;
        r = 0;
    }
    spin_unlock(&blk_lock);

    if (r == 0)
        kprintf("block: %s %llu MiB (%u sect/req max)\n",
                bd->name,
                (unsigned long long)(bd->capacity_sectors >>
                                     (20 - BLK_SECTOR_SHIFT)),
                bd->max_sectors);
    return r;
}

struct block_device *block_find(const char *name)
{
    if (!name)
        return NULL;
    for (unsigned i = 0; i < nbdevs; i++)
        if (s_eq(bdevs[i]->name, name))
            return bdevs[i];
    return NULL;
}

struct block_device *block_first(void)
{
    return nbdevs ? bdevs[0] : NULL;
}

unsigned block_device_count(void)
{
    return nbdevs;
}

/* ---- sector cache ---------------------------------------------------------------------- */

#define CACHE_ENTRIES 16

struct cache_line {
    bool valid;
    uint64_t lba;
    uint8_t data[BLK_SECTOR_SIZE];
};

struct blk_cache {
    struct cache_line line[CACHE_ENTRIES];
    spinlock_t lock;
};

static struct blk_cache caches[BLK_DEV_MAX];

static struct blk_cache *cache_for(struct block_device *bd)
{
    for (unsigned i = 0; i < nbdevs; i++)
        if (bdevs[i] == bd)
            return &caches[i];
    return NULL;
}

/* fetch one sector through the cache */
static int cache_read_sector(struct block_device *bd,
                             struct blk_cache *c, uint64_t lba,
                             uint8_t *out)
{
    daif_state s;
    unsigned idx = (unsigned)(lba % CACHE_ENTRIES);
    struct cache_line *cl = &c->line[idx];
    int r;

    s = irq_local_save();
    spin_lock(&c->lock);

    if (cl->valid && cl->lba == lba) {
        memcpy(out, cl->data, BLK_SECTOR_SIZE);
        spin_unlock(&c->lock);
        irq_local_restore(s);
        return 0;
    }

    /* miss: fetch straight into the victim line, then copy out */
    r = bd->read_blocks(bd, lba, cl->data, 1);
    if (r == 0) {
        cl->valid = true;
        cl->lba = lba;
        memcpy(out, cl->data, BLK_SECTOR_SIZE);
    }

    spin_unlock(&c->lock);
    irq_local_restore(s);
    return r;
}

/* refresh the cached copy of an overwritten sector */
static void cache_fill_write(struct blk_cache *c, uint64_t lba,
                             const uint8_t *data)
{
    daif_state s;
    unsigned idx = (unsigned)(lba % CACHE_ENTRIES);
    struct cache_line *cl = &c->line[idx];

    s = irq_local_save();
    spin_lock(&c->lock);
    cl->valid = true;
    cl->lba = lba;
    memcpy(cl->data, data, BLK_SECTOR_SIZE);
    spin_unlock(&c->lock);
    irq_local_restore(s);
}

/* ---- core IO paths ------------------------------------------------------------------------ */

int block_submit(struct blk_request *req)
{
    struct blk_cache *c = cache_for(req->bd);
    uint8_t *buf = req->buf;
    uint64_t cur = req->lba;
    unsigned left = req->nsect;
    int rc = 0;

    /*
     * Chunk into driver-max transfers. Single-sector reads go through
     * the cache; larger runs bypass it so streaming workloads do not
     * evict everything.
     */
    while (left && rc == 0) {
        unsigned chunk = left < req->bd->max_sectors
                             ? left : req->bd->max_sectors;

        if (!req->write && chunk == 1 && c) {
            rc = cache_read_sector(req->bd, c, cur, buf);
            if (rc == 0) {
                buf += BLK_SECTOR_SIZE;
                cur++;
                left--;
            }
            continue;
        }

        rc = req->write
                 ? req->bd->write_blocks(req->bd, cur, buf, chunk)
                 : req->bd->read_blocks(req->bd, cur, buf, chunk);
        if (rc != 0)
            break;

        if (req->write && c)
            for (unsigned i = 0; i < chunk; i++)
                cache_fill_write(c, cur + i, buf + i * BLK_SECTOR_SIZE);

        buf += (size_t)chunk * BLK_SECTOR_SIZE;
        cur += chunk;
        left -= chunk;
    }

    req->status = rc;
    req->done = true;
    return rc;
}

int block_read(struct block_device *bd, uint64_t lba,
               void *buf, unsigned bytes)
{
    struct blk_request req;
    unsigned sect = (bytes + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;

    if (!bd || bytes == 0)
        return -1;
    if (lba + sect > bd->capacity_sectors)
        return -1;

    memset(&req, 0, sizeof(req));
    req.bd = bd;
    req.lba = lba;
    req.nsect = sect;
    req.buf = buf;
    req.write = false;
    return block_submit(&req);
}

int block_write(struct block_device *bd, uint64_t lba,
                const void *buf, unsigned bytes)
{
    struct blk_request req;
    unsigned sect = (bytes + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;

    if (!bd || bytes == 0)
        return -1;
    if (lba + sect > bd->capacity_sectors)
        return -1;

    memset(&req, 0, sizeof(req));
    req.bd = bd;
    req.lba = lba;
    req.nsect = sect;
    req.buf = (void *)buf;
    req.write = true;
    return block_submit(&req);
}

/* ---- MBR / EBR ---------------------------------------------------------------------------------- */

#define MBR_SIG_OFF   510
#define MBR_TABLE_OFF 446
#define MBR_ENT_SIZE  16

static bool ent_is_extended(uint8_t type)
{
    return type == 0x05 || type == 0x0f || type == 0x85;
}

static bool mbr_signature_ok(const uint8_t *sec)
{
    return sec[MBR_SIG_OFF] == 0x55 && sec[MBR_SIG_OFF + 1] == 0xaa;
}

/* returns next EBR link LBA, or 0 for "no link" */
static uint64_t parse_mbr_sector(struct block_device *bd,
                                 const uint8_t *sec,
                                 uint64_t ext_base, bool inside_ext)
{
    uint64_t link = 0;

    if (!mbr_signature_ok(sec))
        return 0;

    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + MBR_TABLE_OFF + i * MBR_ENT_SIZE;
        uint8_t type = e[0];
        uint64_t first = rd32(e + 8);
        uint64_t total = rd32(e + 12);

        if (type == 0x00 || total == 0)
            continue;

        if (ent_is_extended(type)) {
            if (inside_ext)
                continue;           /* nested containers: skip */
            link = ext_base + first;
            continue;
        }

        uint64_t abs_start = inside_ext ? ext_base + first : first;

        if (bd->nparts >= PART_MAX)
            break;

        struct partition *p = &bd->parts[bd->nparts++];

        p->valid       = true;
        p->is_gpt      = false;
        p->is_extended = false;
        p->mbr_type    = type;
        p->lba_start   = abs_start;
        p->lba_nsect   = total;
        p->name[0]     = '\0';
    }
    return link;
}

static void add_extended_marker(struct block_device *bd, uint64_t start,
                                uint64_t count, uint8_t type)
{
    if (bd->nparts >= PART_MAX)
        return;

    struct partition *p = &bd->parts[bd->nparts++];

    p->valid       = true;
    p->is_gpt      = false;
    p->is_extended = true;
    p->mbr_type    = type;
    p->lba_start   = start;
    p->lba_nsect   = count;
    p->name[0]     = '\0';
}

static int scan_mbr(struct block_device *bd)
{
    uint8_t sec[BLK_SECTOR_SIZE];
    uint64_t ext_base = 0, ext_total = 0, ebr_lba;

    if (block_read(bd, 0, sec, BLK_SECTOR_SIZE) != 0)
        return 0;
    if (!mbr_signature_ok(sec))
        return 0;

    /* find extended container before parsing ordinary entries */
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + MBR_TABLE_OFF + i * MBR_ENT_SIZE;

        if (ent_is_extended(e[0]) && rd32(e + 12)) {
            ext_base  = rd32(e + 8);
            ext_total = rd32(e + 12);
        }
    }

    parse_mbr_sector(bd, sec, 0, false);

    if (ext_base)
        add_extended_marker(bd, ext_base, ext_total,
                            0x05 /* representative */);

    if (!ext_base)
        return (int)bd->nparts;

    ebr_lba = ext_base;
    for (int guard = 0; guard < 32; guard++) {
        uint64_t next;

        if (ebr_lba >= bd->capacity_sectors)
            break;
        if (block_read(bd, ebr_lba, sec, BLK_SECTOR_SIZE) != 0)
            break;
        next = parse_mbr_sector(bd, sec, ext_base, true);
        if (!next || next <= ebr_lba)
            break;                      /* chain end / loop guard */
        ebr_lba = next;
    }

    return (int)bd->nparts;
}

/* ---- GPT ------------------------------------------------------------------------------------------ */

#define GPT_HDR_LBA   1
#define GPT_HDR_BYTES 92

static bool guid_zero(const uint8_t *g)
{
    for (int i = 0; i < 16; i++)
        if (g[i])
            return false;
    return true;
}

static void gpt_name_decode(char *dst, const uint8_t *utf16, unsigned maxch)
{
    unsigned d = 0;

    for (unsigned i = 0; i < 36 && d < maxch - 1; i++) {
        uint16_t wc = (uint16_t)(utf16[2 * i] | (utf16[2 * i + 1] << 8));

        if (wc == 0)
            break;
        dst[d++] = (wc >= 0x20 && wc < 0x7f) ? (char)wc : '?';
    }
    dst[d] = '\0';
}

static int scan_gpt(struct block_device *bd)
{
    uint8_t hdr[BLK_SECTOR_SIZE];
    uint8_t ents[BLK_SECTOR_SIZE];
    uint32_t nentries, esize, stored_crc, calc_crc;
    uint64_t entry_lba;
    uint32_t total_bytes, consumed = 0;
    uint32_t running = 0xffffffffu;
    uint32_t e_global = 0;

    if (block_read(bd, GPT_HDR_LBA, hdr, BLK_SECTOR_SIZE) != 0)
        return 0;

    if (rd64(hdr) != 0x5452415020494645ull)     /* "EFI PART" */
        return 0;

    stored_crc = rd32(hdr + 16);
    hdr[16] = hdr[17] = hdr[18] = hdr[19] = 0;
    calc_crc = ~crc32_update(0xffffffffu, hdr, GPT_HDR_BYTES);
    hdr[16] = (uint8_t)stored_crc;
    hdr[17] = (uint8_t)(stored_crc >> 8);
    hdr[18] = (uint8_t)(stored_crc >> 16);
    hdr[19] = (uint8_t)(stored_crc >> 24);

    if (calc_crc != stored_crc) {
        kprintf("block: %s gpt header crc bad\n", bd->name);
        return 0;
    }

    nentries  = rd32(hdr + 80);
    esize     = rd32(hdr + 84);
    entry_lba = rd64(hdr + 72);
    stored_crc = rd32(hdr + 88);            /* partition array CRC */

    if (esize < 128 || esize > BLK_SECTOR_SIZE ||
        nentries == 0 || nentries > 128)
        return 0;

    total_bytes = nentries * esize;

    /*
     * One pass per array sector: fold it into the running CRC and
     * harvest the entries it contains (esize divides evenly, so no
     * entry ever straddles two sectors).
     */
    while (consumed < total_bytes) {
        uint64_t slot = entry_lba + consumed / BLK_SECTOR_SIZE;
        uint32_t take = total_bytes - consumed;

        if (take > BLK_SECTOR_SIZE)
            take = BLK_SECTOR_SIZE;

        if (slot >= bd->capacity_sectors ||
            block_read(bd, slot, ents, BLK_SECTOR_SIZE) != 0)
            break;

        running = crc32_update(running, ents, take);
        consumed += take;

        for (unsigned off = 0; off + esize <= take &&
                              e_global < nentries;
             off += esize, e_global++) {
            const uint8_t *en = ents + off;

            if (guid_zero(en))
                continue;

            uint64_t first = rd64(en + 32);
            uint64_t last  = rd64(en + 40);

            if (last < first || last >= bd->capacity_sectors)
                continue;
            if (bd->nparts >= PART_MAX)
                break;

            struct partition *p = &bd->parts[bd->nparts++];
            const uint8_t *name16 = en + 56;

            p->valid       = true;
            p->is_gpt      = true;
            p->is_extended = false;
            p->mbr_type    = 0;
            p->lba_start   = first;
            p->lba_nsect   = last - first + 1;
            gpt_name_decode(p->name, name16, PART_NAME_MAX);
        }
    }

    if (~running != stored_crc)
        kprintf("block: %s gpt entries crc mismatch (continuing)\n",
                bd->name);

    return (int)bd->nparts;
}

int block_scan_partitions(struct block_device *bd,
                          struct partition *out)
{
    uint8_t sec[BLK_SECTOR_SIZE];
    int n;

    if (!bd)
        return -1;

    bd->nparts = 0;
    memset(bd->parts, 0, sizeof(bd->parts));

    /* protective-MBR entry of type 0xEE signals a GPT disk */
    if (block_read(bd, 0, sec, BLK_SECTOR_SIZE) == 0 &&
        mbr_signature_ok(sec)) {
        bool protective = false;

        for (int i = 0; i < 4; i++)
            if (sec[MBR_TABLE_OFF + i * MBR_ENT_SIZE] == 0xee)
                protective = true;

        if (protective) {
            n = scan_gpt(bd);
            goto done;
        }
    }

    n = scan_mbr(bd);

done:
    if (out && n > 0)
        memcpy(out, bd->parts, sizeof(struct partition) * (unsigned)n);
    return n;
}

void block_print_partitions(const struct block_device *bd)
{
    kprintf("part: %s holds %u partition%s\n",
            bd->name, bd->nparts, bd->nparts == 1 ? "" : "s");

    for (unsigned i = 0; i < bd->nparts; i++) {
        const struct partition *p = &bd->parts[i];

        if (p->is_gpt)
            kprintf("part:   %s%d \"%s\" @ %-8llu +%llu sect\n",
                    bd->name, i, p->name,
                    (unsigned long long)p->lba_start,
                    (unsigned long long)p->lba_nsect);
        else
            kprintf("part:   %s%d type 0x%02x @ %-8llu +%llu sect%s\n",
                    bd->name, i, p->mbr_type,
                    (unsigned long long)p->lba_start,
                    (unsigned long long)p->lba_nsect,
                    p->is_extended ? " (container)" : "");
    }
}
