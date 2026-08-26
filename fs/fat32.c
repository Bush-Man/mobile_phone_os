/*
 * fat32.c - FAT32 filesystem with read AND write support (phase 7).
 *
 * Layout facts used everywhere below:
 *   - logical sectors are 512 bytes (the block layer constant),
 *   - cluster c data starts at partition-relative sector
 *     data_start + (c-2)*sec_per_clus,
 *   - FAT entry n lives in FAT-relative sector reserved + n/128 at
 *     byte (n%128)*4, mirrored across nfats copies,
 *   - directory entries are 32 bytes; directories grow cluster-wise
 *     and never shrink,
 *   - long file names ride in 0x0F-attribute entries physically
 *     ahead of their short entry (the head carries ord | 0x40 and
 *     ords descend toward the short name); deletions mark the first
 *     byte 0xE5.
 *
 * Locking: the virtio block path sleeps, so a per-instance
 * SPINLOCK around metadata would deadlock the machine. Instead every
 * vnode operation wraps itself in a sleeping "big fs lock" (a
 * test-and-set flag plus phase-4 wait queue -- syscalls always run
 * in task context here, never in IRQ context). Internal helpers
 * simply assume it is held; nothing may re-acquire recursively.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "block.h"
#include "fat32.h"
#include "lib.h"
#include "mm/kheap.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"

#define FAT_EOC       0x0FFFFFF8u       /* >= this == end of chain   */
#define ATTR_LFN      0x0Fu
#define ATTR_DIR      0x10u
#define SLOT_FREE     0x00u             /* also end-of-directory     */
#define SLOT_DELETED  0xE5u

#define FAT_MAX_FILE  (8u * 1024u * 1024u)     /* sanity cap         */
#define DOS_DATE ((uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1))
#define DOS_TIME ((uint16_t)0)

const struct vnode_ops fat_vops;

/* ---- little-endian helpers --------------------------------------------------- */

static inline uint16_t rd16(const uint8_t *b, unsigned off)
{
    return (uint16_t)(b[off] | (b[off + 1] << 8));
}

static inline uint32_t rd32(const uint8_t *b, unsigned off)
{
    return b[off] | (b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) |
           ((uint32_t)b[off + 3] << 24);
}

static inline void wr16(uint8_t *b, unsigned off, uint16_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
}

static inline void wr32(uint8_t *b, unsigned off, uint32_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

/* ---- core types ------------------------------------------------------------------ */

/* sleeping mutex: safe to hold across blocking block-layer calls  */
struct fat_sem {
    spinlock_t lk;                      /* guards ->held             */
    volatile bool held;
    struct waitqueue wq;
};

struct fat_fs {
    struct block_device *bd;
    uint64_t part_lba;
    uint64_t part_nsect;

    uint32_t sec_per_clus;
    uint32_t reserved;
    uint32_t nfats;
    uint32_t fatsz;                     /* sectors per FAT copy      */
    uint32_t root_clus;
    uint32_t data_start;                /* rel. sector of cluster 2  */
    uint32_t total_clusters;

    struct fat_sem sem;
};

struct fat_inode {
    struct fat_fs *fs;
    uint32_t first_clus;                /* 0 = empty file            */
    bool is_dir;
    uint64_t size;

    /* SFN dirent location, needed to update size/cluster later     */
    bool has_dirent;
    uint64_t de_lba;
    unsigned de_off;
};

/* position inside one cluster chain                                */
struct fat_cursor {
    uint32_t clus;
    uint32_t idx;
};

static bool bfl_busy(void *ctx)
{
    return ((struct fat_sem *)ctx)->held;
}

static void sem_acquire(struct fat_sem *sem)
{
    daif_state s;

    for (;;) {
        spin_lock_irqsave(&sem->lk, &s);
        if (!sem->held) {
            sem->held = true;
            spin_unlock_irqrestore(&sem->lk, s);
            return;
        }
        spin_unlock_irqrestore(&sem->lk, s);

        /*
         * Sleeps while ->held; the predicate is re-evaluated fresh
         * under the scheduler lock, and the outer loop re-checks,
         * so releases are never lost beyond one spurious round.
         */
        wait_sleep_when(bfl_busy, sem, &sem->wq);
    }
}

static void sem_release(struct fat_sem *sem)
{
    daif_state s;

    spin_lock_irqsave(&sem->lk, &s);
    sem->held = false;
    spin_unlock_irqrestore(&sem->lk, s);
    wait_wake_all(&sem->wq);
}

static inline uint32_t clus_bytes(const struct fat_fs *fs)
{
    return fs->sec_per_clus * 512u;
}

static inline uint64_t clus_rel_lba(const struct fat_fs *fs, uint32_t c)
{
    return fs->data_start + (uint64_t)(c - 2) * fs->sec_per_clus;
}

static int sec_read(const struct fat_fs *fs, uint64_t rel_lba,
                    void *buf)
{
    return block_read(fs->bd, fs->part_lba + rel_lba, buf, 512);
}

static int sec_write(const struct fat_fs *fs, uint64_t rel_lba,
                     const void *buf)
{
    return block_write(fs->bd, fs->part_lba + rel_lba, buf, 512);
}

static bool pow2(unsigned v)
{
    return v && !(v & (v - 1));
}

static char upchar(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* ---- FAT chain -------------------------------------------------------------------- */

static int fat_entry(struct fat_fs *fs, uint32_t n, uint32_t *out)
{
    uint8_t sec[512];
    int r;

    if (n < 2 || n >= fs->total_clusters + 2)
        return -EIO;
    r = sec_read(fs, fs->reserved + n / 128, sec);
    if (r)
        return r;
    *out = rd32(sec, (n % 128) * 4);
    return 0;
}

/* updates EVERY FAT mirror so both copies stay identical          */
static int fat_set_entry(struct fat_fs *fs, uint32_t n, uint32_t val)
{
    uint8_t sec[512];
    uint64_t rel = fs->reserved + n / 128;
    int r;

    r = sec_read(fs, rel, sec);
    if (r)
        return r;
    wr32(sec, (n % 128) * 4, val);
    for (uint32_t i = 0; i < fs->nfats; i++) {
        r = sec_write(fs, rel + (uint64_t)i * fs->fatsz, sec);
        if (r)
            return r;
    }
    return 0;
}

static bool is_eoc(uint32_t v)
{
    return v >= FAT_EOC && v <= 0x0FFFFFFFu;
}

/* number of clusters in a chain (0 when unallocated)              */
static int chain_len(struct fat_fs *fs, uint32_t first, uint32_t *out)
{
    uint32_t c = first, n = 0;
    int r;

    while (c >= 2 && !is_eoc(c)) {
        r = fat_entry(fs, c, &c);
        if (r)
            return r;
        if (++n > fs->total_clusters)
            return -EIO;                /* loop guard                 */
    }
    *out = n;
    return 0;
}

/* advance a cursor along the chain to absolute index `want`       */
static int chain_seek(struct fat_fs *fs, struct fat_cursor *cur,
                      uint32_t want, uint32_t *out)
{
    int r;

    if (cur->clus < 2)
        return -EIO;
    while (cur->idx < want) {
        uint32_t next;

        r = fat_entry(fs, cur->clus, &next);
        if (r)
            return r;
        if (!next || is_eoc(next))
            return -EIO;
        cur->clus = next;
        cur->idx++;
    }
    *out = cur->clus;
    return 0;
}

/* cluster # of the idx-th link of a chain starting at `first`     */
static int chain_nth(struct fat_fs *fs, uint32_t first,
                     uint32_t idx, uint32_t *out)
{
    struct fat_cursor cur = { first, 0 };

    return chain_seek(fs, &cur, idx, out);
}

/*
 * Append count zeroed clusters to the chain whose head is *first
 * (a head of 0 becomes the new first cluster). Caller holds sem.
 */
static int chain_grow(struct fat_fs *fs, uint32_t *first,
                      uint32_t count)
{
    uint8_t zero[512];
    uint32_t prev = *first, added = 0;
    int r;

    memset(zero, 0, sizeof(zero));

    if (prev >= 2) {
        for (;;) {                      /* walk to the current tail  */
            uint32_t next;

            r = fat_entry(fs, prev, &next);
            if (r)
                return r;
            if (!next || is_eoc(next))
                break;
            prev = next;
        }
    }

    while (added < count) {
        uint32_t fresh = 0;

        for (uint32_t scn = 2; scn < fs->total_clusters + 2; scn++) {
            uint32_t v;

            r = fat_entry(fs, scn, &v);
            if (r)
                return r;
            if (v == 0) {
                fresh = scn;
                break;
            }
        }
        if (!fresh)
            return -ENOSPC;

        for (uint32_t i = 0; i < fs->sec_per_clus; i++) {
            r = sec_write(fs, clus_rel_lba(fs, fresh) + i, zero);
            if (r)
                return r;
        }
        r = fat_set_entry(fs, fresh, FAT_EOC);
        if (r)
            return r;
        if (prev >= 2) {
            r = fat_set_entry(fs, prev, fresh);
            if (r)
                return r;
        } else {
            *first = fresh;             /* chain was empty           */
        }
        prev = fresh;
        added++;
    }
    return 0;
}

static int chain_free(struct fat_fs *fs, uint32_t first)
{
    uint32_t c = first;
    int r;

    while (c >= 2 && !is_eoc(c)) {
        uint32_t next;

        r = fat_entry(fs, c, &next);
        if (r)
            return r;
        r = fat_set_entry(fs, c, 0);
        if (r)
            return r;
        c = next;
    }
    return 0;
}

/* ---- directory slots --------------------------------------------------------------- */

/*
 * Physical slot addressing across the whole chain: slot p lives in
 * chain-cluster p/(slots_per_cluster) at byte offset
 * (p % slots_per_cluster)*32. clus_out is optional.
 */
static int dir_slot_locate(struct fat_fs *fs, const struct fat_inode *di,
                           uint32_t pidx, uint32_t *clus_out,
                           uint64_t *lba, unsigned *off)
{
    uint32_t spc_slots = clus_bytes(fs) / 32u;
    uint32_t ci = pidx / spc_slots;
    uint32_t coff = (pidx % spc_slots) * 32u;
    uint32_t c;
    int r;

    r = chain_nth(fs, di->first_clus, ci, &c);
    if (r)
        return r;
    if (!c)
        return -EIO;                    /* past the chain             */
    if (clus_out)
        *clus_out = c;
    *lba = clus_rel_lba(fs, c) + (coff >> 9);
    *off = coff & 511;
    return 0;
}

static int dir_read_slot(struct fat_fs *fs, const struct fat_inode *di,
                         uint32_t pidx, uint8_t *slot32 /* 32 */)
{
    uint64_t lba;
    unsigned off;
    uint8_t sec[512];
    int r;

    r = dir_slot_locate(fs, di, pidx, NULL, &lba, &off);
    if (r)
        return r;
    r = sec_read(fs, lba, sec);
    if (r)
        return r;
    memcpy(slot32, &sec[off], 32);
    return 0;
}

static int dir_write_slot(struct fat_fs *fs, const struct fat_inode *di,
                          uint32_t pidx, const uint8_t *slot32)
{
    uint64_t lba;
    unsigned off;
    uint8_t sec[512];
    int r;

    r = dir_slot_locate(fs, di, pidx, NULL, &lba, &off);
    if (r)
        return r;
    r = sec_read(fs, lba, sec);
    if (!r) {
        memcpy(&sec[off], slot32, 32);
        r = sec_write(fs, lba, sec);
    }
    return r;
}

/* ---- long file names --------------------------------------------------------------- */

static uint8_t lfn_checksum(const char sfn[11])
{
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++)
        sum = (uint8_t)((sum >> 1) | (sum << 7)) +
              (uint8_t)(unsigned char)sfn[i];
    return sum;
}

struct fat_dirent_info {
    char name[VFS_NAME_MAX];            /* long name when present    */
    char sfn[12];
    uint8_t attr;
    uint32_t first_clus;
    uint64_t size;
    uint32_t pidx;                      /* physical slot of the SFN  */
};

static void decode_sfn(const char sfn[11], char *out /* >= 13 */)
{
    unsigned nb = 8, ne = 3, o = 0;

    while (nb > 0 && sfn[nb - 1] == ' ')
        nb--;
    while (ne > 0 && sfn[8 + ne - 1] == ' ')
        ne--;

    for (unsigned i = 0; i < nb; i++)
        out[o++] = sfn[i];
    if (ne) {
        out[o++] = '.';
        for (unsigned i = 0; i < ne; i++)
            out[o++] = sfn[8 + i];
    }
    out[o] = '\0';
}

/*
 * Fetch the entry at *pidx_io and advance past it (including any
 * LFN run). Returns 1 = valid entry in `info`, 0 = end-of-directory
 * marker reached, negative errno otherwise.
 */
static int dir_next(struct fat_fs *fs, struct fat_inode *di,
                    uint32_t *pidx_io, struct fat_dirent_info *info)
{
    uint16_t lfn[VFS_NAME_MAX];
    unsigned lfn_parts = 0;
    uint8_t lfn_sum = 0;
    uint32_t pidx = *pidx_io;

    memset(info, 0, sizeof(*info));

    for (;; pidx++) {
        uint8_t e[32];
        int r = dir_read_slot(fs, di, pidx, e);

        if (r == -EIO)
            return 0;                   /* ran past the chain         */
        if (r)
            return r;

        if (e[0] == SLOT_FREE)
            return 0;

        if (e[0] == SLOT_DELETED)
            continue;                   /* tombstones invisible       */

        if (e[11] == ATTR_LFN) {
            unsigned ord = e[0] & 0x1Fu;
            unsigned base;

            if (e[0] & 0x40) {          /* head: restart collection   */
                lfn_parts = ord;
                lfn_sum = e[13];
                for (unsigned i = 0; i < VFS_NAME_MAX; i++)
                    lfn[i] = 0xFFFF;
            }
            if (!ord || ord > lfn_parts)
                continue;
            base = (ord - 1) * 13u;
            for (unsigned k = 0; k < 13; k++) {
                unsigned src = k < 5 ? 1 + k * 2
                             : k < 11 ? 14 + (k - 5) * 2
                                      : 28 + (k - 11) * 2;

                if (base + k < VFS_NAME_MAX)
                    lfn[base + k] = rd16(e, src);
            }
            continue;
        }

        /* regular entry                                           */
        memcpy(info->sfn, e, 11);
        info->sfn[11] = '\0';
        info->attr = e[11];
        info->first_clus = rd16(e, 26) |
                           ((uint32_t)rd16(e, 20) << 16);
        info->size = info->attr & ATTR_DIR ? 0 : rd32(e, 28);
        info->pidx = pidx;
        *pidx_io = pidx + 1;

        if (lfn_parts && lfn_checksum(info->sfn) == lfn_sum) {
            unsigned o = 0;

            for (unsigned i = 0;
                 i < lfn_parts * 13u && o < VFS_NAME_MAX - 1; i++) {
                uint16_t ch = lfn[i];

                if (ch == 0xFFFF || ch == 0)
                    break;
                info->name[o++] = ch < 128 ? (char)ch : '?';
            }
            info->name[o] = '\0';
        } else {
            decode_sfn(info->sfn, info->name);
        }
        return 1;
    }
}

/* ---- names --------------------------------------------------------------------------- */

static bool sfn_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '!' || c == '#' || c == '$' || c == '%' ||
           c == '&' || c == '\'' || c == '(' || c == ')' ||
           c == '-' || c == '@' || c == '^' || c == '_' ||
           c == '`' || c == '{' || c == '}' || c == '~';
}

static const char *last_dot(const char *s)
{
    const char *found = NULL;

    for (; *s; s++)
        if (*s == '.')
            found = s;
    return found;
}

/* expressible as exact uppercase 8.3 (no LFN entries needed)?     */
static bool plain_sfn(const char *name)
{
    const char *dot = NULL;
    unsigned nb, ne;

    for (const char *s = name; *s; s++) {
        if (*s == '.') {
            if (dot)
                return false;
            dot = s;
        } else if (!sfn_char_ok(*s)) {
            return false;
        }
    }
    if (!*name)
        return false;
    nb = dot ? (unsigned)(dot - name) : (unsigned)strlen(name);
    ne = dot ? (unsigned)strlen(dot + 1) : 0;
    return nb >= 1 && nb <= 8 && ne <= 3 && (!dot || ne >= 1);
}

/*
 * Build an 11-byte SFN template for `name`. When the name does not
 * fit exactly, position *tail_at marks where a decimal digit
 * (tried 1..9) replaces the '~'. Returns true when a numeric tail
 * is required.
 */
static bool make_sfn_template(const char *name, char out[11],
                              unsigned *tail_at)
{
    const char *dot = last_dot(name);

    memset(out, ' ', 11);

    if (plain_sfn(name)) {
        unsigned nb = dot ? (unsigned)(dot - name)
                          : (unsigned)strlen(name);
        unsigned ne = dot ? (unsigned)strlen(dot + 1) : 0;

        for (unsigned i = 0; i < nb; i++)
            out[i] = upchar(name[i]);
        for (unsigned i = 0; i < ne; i++)
            out[8 + i] = upchar(dot[1 + i]);
        *tail_at = 0;
        return false;
    }

    {
        unsigned nb = 0, ne = 0;

        for (const char *s = name; *s && (!dot || s < dot) &&
             nb < 6; s++)
            if (sfn_char_ok(upchar(*s)))
                out[nb++] = upchar(*s);
        if (!nb)
            out[nb++] = '_';
        out[nb++] = '~';
        *tail_at = nb;
        if (dot) {
            for (const char *s = dot + 1; *s && ne < 3; s++)
                if (sfn_char_ok(upchar(*s)))
                    out[8 + ne++] = upchar(*s);
        }
        return true;
    }
}

static bool name_eq(const struct fat_dirent_info *info,
                    const char *name)
{
    char flat[13];
    unsigned i;

    if (strcmp(info->name, name) == 0)
        return true;
    decode_sfn(info->sfn, flat);
    if (strlen(flat) != strlen(name))
        return false;
    for (i = 0; name[i]; i++)
        if (upchar(name[i]) != flat[i])
            return false;
    return true;
}

/* ---- dirent field sync ------------------------------------------------------------------ */

/* caller holds sem                                                */
static int dirent_sync(struct fat_inode *in)
{
    struct fat_fs *fs = in->fs;
    uint8_t sec[512];
    int r;

    if (!in->has_dirent)
        return 0;
    r = sec_read(fs, in->de_lba, sec);
    if (r)
        return r;
    wr16(sec, in->de_off + 26,
         (uint16_t)(in->first_clus & 0xFFFF));
    wr16(sec, in->de_off + 20,
         (uint16_t)(in->first_clus >> 16));
    wr32(sec, in->de_off + 28, (uint32_t)in->size);
    return sec_write(fs, in->de_lba, sec);
}

/* ---- inode shells ------------------------------------------------------------------------------ */

static struct vnode *inode_shell(struct fat_inode *in)
{
    struct vnode *vn = kzalloc(sizeof(*vn));

    if (!vn)
        return NULL;
    vn->ops = &fat_vops;
    vn->type = in->is_dir ? V_DIR : V_FILE;
    vn->priv = in;
    vn->ino = in->first_clus;
    return vn;
}

static void fat_destroy(struct vnode *vn)
{
    kfree(vn->priv);
    kfree(vn);
}

/* ---- lookup / readdir ------------------------------------------------------------------------- */

static int fat_lookup(struct vnode *dir, const char *name,
                      struct vnode **out)
{
    struct fat_inode *d = dir->priv;
    struct fat_fs *fs = d->fs;
    struct fat_dirent_info info;
    struct fat_inode *in;
    uint32_t pidx = 0;
    uint64_t lba;
    unsigned off;
    int r;

    if (!name[0])
        return -ENOENT;

    sem_acquire(&fs->sem);

    for (;;) {
        r = dir_next(fs, d, &pidx, &info);
        if (r <= 0) {
            sem_release(&fs->sem);
            return r == 0 ? -ENOENT : r;
        }
        if (info.attr & 0x08)
            continue;                   /* volume label               */
        if (!name_eq(&info, name))
            continue;

        r = dir_slot_locate(fs, d, info.pidx, NULL, &lba, &off);
        if (r) {
            sem_release(&fs->sem);
            return r;
        }

        in = kzalloc(sizeof(*in));
        if (!in) {
            sem_release(&fs->sem);
            return -ENOMEM;
        }
        in->fs = fs;
        in->is_dir = !!(info.attr & ATTR_DIR);
        in->first_clus = info.first_clus;
        in->size = info.size;
        in->has_dirent = true;
        in->de_lba = lba;
        in->de_off = off;

        *out = inode_shell(in);
        sem_release(&fs->sem);
        if (!*out) {
            kfree(in);
            return -ENOMEM;
        }
        return 0;
    }
}

static int fat_readdir(struct vnode *dir, unsigned idx,
                       char *name_out, uint8_t *type_out)
{
    struct fat_inode *d = dir->priv;
    struct fat_fs *fs = d->fs;
    struct fat_dirent_info info;
    uint32_t pidx = 0;
    int r;

    sem_acquire(&fs->sem);

    for (;;) {
        r = dir_next(fs, d, &pidx, &info);
        if (r <= 0) {
            sem_release(&fs->sem);
            return r == 0 ? -ENOENT : r;
        }
        if (info.attr & 0x08)
            continue;                   /* volume label               */
        if (strcmp(info.name, ".") == 0 ||
            strcmp(info.name, "..") == 0)
            continue;                   /* hide dot entries           */
        if (!idx--)
            break;
    }

    kstrlcpy(name_out, info.name, VFS_NAME_MAX);
    *type_out = info.attr & ATTR_DIR ? DT_DIR : DT_REG;
    sem_release(&fs->sem);
    return 0;
}

/* ---- create / unlink -------------------------------------------------------------------------------------------- */

/* consecutive free/deleted slots from pidx within [0,cap)?        */
static int free_run(struct fat_fs *fs, struct fat_inode *d,
                    uint32_t from, uint32_t cap,
                    uint32_t *run_out, bool *past_chain)
{
    uint32_t run = 0;

    *past_chain = false;
    for (uint32_t p = from; p < cap; p++) {
        uint8_t e[32];
        int r = dir_read_slot(fs, d, p, e);

        if (r == -EIO) {                /* past chain: virtual zeros  */
            *past_chain = true;
            break;
        }
        if (r)
            return r;
        if (e[0] != SLOT_FREE && e[0] != SLOT_DELETED)
            break;
        run++;
    }
    *run_out = run;
    return 0;
}

static int fat_create(struct vnode *dir, const char *name,
                      enum vtype t, struct vnode **out)
{
    struct fat_inode *d = dir->priv;
    struct fat_fs *fs = d->fs;
    struct fat_dirent_info info;
    struct fat_inode *in = NULL;
    struct vnode *vn;
    char tmpl[11], sfn[11];
    unsigned tail_at, lfn_count;
    bool need_tail;
    uint32_t pidx, cap_slots, run, nchain = 0;
    bool past_chain;
    int r;

    if (strlen(name) >= VFS_NAME_MAX)
        return -ENAMETOOLONG;

    need_tail = make_sfn_template(name, tmpl, &tail_at);
    lfn_count = plain_sfn(name)
              ? 0 : (unsigned)((strlen(name) + 12) / 13);

    sem_acquire(&fs->sem);

    /* pick a short name that does not collide                      */
    for (int attempt = need_tail ? 1 : 0;; attempt++) {
        bool clash = false;

        memcpy(sfn, tmpl, 11);
        if (need_tail) {
            if (attempt > 9) {
                sem_release(&fs->sem);
                return -EEXIST;
            }
            sfn[tail_at] = (char)('0' + attempt);
        }

        pidx = 0;
        for (;;) {
            r = dir_next(fs, d, &pidx, &info);
            if (r < 0) {
                sem_release(&fs->sem);
                return r;
            }
            if (!r)
                break;
            if (memcmp(info.sfn, sfn, 11) == 0) {
                clash = true;
                break;
            }
        }
        if (!clash)
            break;
    }

    in = kzalloc(sizeof(*in));
    if (!in) {
        sem_release(&fs->sem);
        return -ENOMEM;
    }
    in->fs = fs;
    in->is_dir = t == V_DIR;

    /* directories claim their first cluster now and get . / ..     */
    if (in->is_dir) {
        uint32_t self = 0;

        r = chain_grow(fs, &self, 1);
        if (r)
            goto fail;
        in->first_clus = self;

        {
            uint8_t sec[512];
            uint64_t base = clus_rel_lba(fs, self);

            r = sec_read(fs, base, sec);
            if (r)
                goto fail;
            memset(sec, 0, 64);
            memcpy(sec, ".          ", 11);
            memcpy(&sec[32], "..         ", 11);
            sec[11] = sec[43] = ATTR_DIR;
            wr16(sec, 22, DOS_TIME);
            wr16(sec, 24, DOS_DATE);
            wr16(sec, 54, DOS_TIME);
            wr16(sec, 56, DOS_DATE);
            wr16(sec, 26, (uint16_t)(self & 0xFFFF));
            wr16(sec, 20, (uint16_t)(self >> 16));
            wr16(sec, 58, (uint16_t)(d->first_clus & 0xFFFF));
            wr16(sec, 52, (uint16_t)(d->first_clus >> 16));

            r = sec_write(fs, base, sec);
            if (r)
                goto fail;
        }
    }

    /* find room for lfn_count + 1 consecutive slots                */
    if (d->first_clus)
        chain_len(fs, d->first_clus, &nchain);
    cap_slots = nchain * (clus_bytes(fs) / 32u);

    pidx = 0;
    for (;;) {
        uint32_t slots_per_clus = clus_bytes(fs) / 32u;

        r = free_run(fs, d, pidx, cap_slots, &run, &past_chain);
        if (r)
            goto fail;
        if (run >= lfn_count + 1)
            break;

        if (past_chain || pidx >= cap_slots) {
            uint32_t add = d->first_clus;

            r = chain_grow(fs, &add, 1);
            if (r)
                goto fail;
            if (!d->first_clus)
                d->first_clus = add;    /* dir previously empty       */
            pidx = cap_slots;
            cap_slots += slots_per_clus;
            continue;
        }
        pidx += run ? run : 1;
    }

    /* write the slot run: LFN fragments descending, SFN last       */
    if (lfn_count) {
        uint8_t sum = lfn_checksum(sfn);
        unsigned total = (unsigned)strlen(name);

        for (unsigned ord = lfn_count; ord >= 1; ord--) {
            uint8_t e[32];
            unsigned base = (ord - 1) * 13u;

            memset(e, 0, 32);
            e[0] = (uint8_t)(ord == lfn_count ?
                             (0x40 | lfn_count) : ord);
            e[11] = ATTR_LFN;
            e[13] = sum;
            for (unsigned k = 0; k < 13; k++) {
                uint16_t ch = base + k < total
                              ? (uint16_t)(unsigned char)
                                name[base + k]
                              : 0xFFFF;
                unsigned dst = k < 5 ? 1 + k * 2
                             : k < 11 ? 14 + (k - 5) * 2
                                      : 28 + (k - 11) * 2;

                wr16(e, dst, ch);
            }
            r = dir_write_slot(fs, d, pidx + (lfn_count - ord), e);
            if (r)
                goto fail;
        }
    }

    {
        uint8_t e[32];

        memset(e, 0, 32);
        memcpy(e, sfn, 11);
        e[11] = in->is_dir ? ATTR_DIR : 0x20;       /* archive bit  */
        wr16(e, 14, DOS_TIME);
        wr16(e, 16, DOS_DATE);
        wr16(e, 18, DOS_DATE);
        wr16(e, 22, DOS_TIME);
        wr16(e, 24, DOS_DATE);
        wr16(e, 26, (uint16_t)(in->first_clus & 0xFFFF));
        wr16(e, 20, (uint16_t)(in->first_clus >> 16));
        wr32(e, 28, 0);

        r = dir_write_slot(fs, d, pidx + lfn_count, e);
        if (r)
            goto fail;
    }

    {
        uint64_t lba;
        unsigned off;

        r = dir_slot_locate(fs, d, pidx + lfn_count, NULL,
                            &lba, &off);
        if (r)
            goto fail;
        in->has_dirent = true;
        in->de_lba = lba;
        in->de_off = off;
    }

    vn = inode_shell(in);
    if (!vn) {
        r = -ENOMEM;
        goto fail;
    }
    sem_release(&fs->sem);
    *out = vn;
    return 0;

fail:
    if (in) {
        if (in->is_dir && in->first_clus)
            chain_free(fs, in->first_clus);
        kfree(in);
    }
    sem_release(&fs->sem);
    return r;
}

static bool dir_is_empty(struct fat_fs *fs, struct fat_inode *d)
{
    struct fat_dirent_info info;
    uint32_t pidx = 0;

    for (;;) {
        int r = dir_next(fs, d, &pidx, &info);

        if (r <= 0)
            return true;
        if (info.attr & 0x08)
            continue;
        if (strcmp(info.name, ".") == 0 ||
            strcmp(info.name, "..") == 0)
            continue;
        return false;
    }
}

static int fat_unlink(struct vnode *dir, const char *name)
{
    struct fat_inode *d = dir->priv;
    struct fat_fs *fs = d->fs;
    struct fat_dirent_info info;
    uint32_t pidx = 0;
    uint8_t e[32];
    int r;

    sem_acquire(&fs->sem);

    for (;;) {
        r = dir_next(fs, d, &pidx, &info);
        if (r <= 0) {
            sem_release(&fs->sem);
            return r == 0 ? -ENOENT : r;
        }
        if (info.attr & 0x08)
            continue;
        if (strcmp(info.name, ".") == 0 ||
            strcmp(info.name, "..") == 0)
            continue;
        if (name_eq(&info, name))
            break;
    }

    if (info.attr & ATTR_DIR) {
        struct fat_inode sub = {
            .fs = fs,
            .first_clus = info.first_clus,
            .is_dir = true,
        };

        if (!dir_is_empty(fs, &sub)) {
            sem_release(&fs->sem);
            return -ENOTEMPTY;
        }
    } else if (info.first_clus >= 2) {
        chain_free(fs, info.first_clus);        /* release data     */
    }

    r = dir_read_slot(fs, d, info.pidx, e);
    if (!r) {
        e[0] = SLOT_DELETED;
        r = dir_write_slot(fs, d, info.pidx, e);
    }
    if (r) {
        sem_release(&fs->sem);
        return r;
    }

    for (uint32_t p = info.pidx; p > 0;) {
        p--;
        r = dir_read_slot(fs, d, p, e);
        if (r || e[11] != ATTR_LFN)
            break;
        e[0] = SLOT_DELETED;
        r = dir_write_slot(fs, d, p, e);
        if (r)
            break;
    }

    sem_release(&fs->sem);
    return 0;
}

/* ---- data ------------------------------------------------------------------------------------------------------------- */

static long fat_read(struct vnode *vn, uint64_t off,
                    void *buf, size_t len)
{
    struct fat_inode *in = vn->priv;
    struct fat_fs *fs = in->fs;
    struct fat_cursor cur;
    uint8_t sec[512];
    size_t done = 0;
    int r;

    if (vn->type == V_DIR)
        return -EISDIR;

    sem_acquire(&fs->sem);

    if (off >= in->size || !len) {
        sem_release(&fs->sem);
        return 0;
    }
    if ((uint64_t)len > in->size - off)
        len = (size_t)(in->size - off);

    cur.clus = in->first_clus;
    cur.idx = 0;

    while (done < len) {
        uint32_t want = (uint32_t)((off + done) / clus_bytes(fs));
        size_t coff = (off + done) % clus_bytes(fs);
        size_t chunk = clus_bytes(fs) - coff;
        uint32_t c;

        if (!cur.clus)
            break;                      /* empty-file EOF             */

        r = chain_seek(fs, &cur, want, &c);
        if (r) {
            sem_release(&fs->sem);
            return done ? (long)done : r;
        }
        r = sec_read(fs, clus_rel_lba(fs, c) + (coff >> 9), sec);
        if (r) {
            sem_release(&fs->sem);
            return done ? (long)done : r;
        }
        if (chunk > len - done)
            chunk = len - done;
        memcpy((uint8_t *)buf + done, &sec[coff & 511], chunk);
        done += chunk;
    }

    sem_release(&fs->sem);
    return (long)done;
}

/* truncate to zero: free the chain, clear dirent fields           */
static int fat_truncate(struct fat_inode *in)
{
    int r = in->first_clus ?
            chain_free(in->fs, in->first_clus) : 0;

    if (r)
        return r;
    in->first_clus = 0;
    in->size = 0;
    return dirent_sync(in);
}

/*
 * Copy buf into [off, off+len), growing the chain as needed and
 * zero-filling any hole between the old EOF and off. Caller holds
 * sem. (Split out of fat_write so hole-filling cannot recurse into
 * the locking wrapper.)
 */
static long fat_write_range(struct fat_inode *in, uint64_t off,
                           const void *buf, size_t len)
{
    struct fat_fs *fs = in->fs;
    struct fat_cursor cur;
    uint8_t sec[512];
    size_t done = 0;
    int r;

    if (off + len > FAT_MAX_FILE)
        return -ENOSPC;

    {
        uint32_t need = (uint32_t)((off + len + clus_bytes(fs) - 1) /
                                   clus_bytes(fs));
        uint32_t have = 0;

        if (in->first_clus) {
            r = chain_len(fs, in->first_clus, &have);
            if (r)
                return r;
        }
        if (have < need) {
            r = chain_grow(fs, &in->first_clus, need - have);
            if (r)
                return r;
        }
    }

    /* fill the hole [old_size, off) with zeros                     */
    if (off > in->size) {
        static uint8_t zeros[512];
        uint64_t pos = in->size;

        memset(zeros, 0, sizeof(zeros));
        while (pos < off) {
            uint64_t coff = pos % clus_bytes(fs);
            size_t chunk = clus_bytes(fs) - (size_t)coff;
            uint32_t want = (uint32_t)(pos / clus_bytes(fs)), c;

            if (chunk > off - pos)
                chunk = (size_t)(off - pos);

            r = chain_nth(fs, in->first_clus, want, &c);
            if (r)
                return r;
            r = sec_read(fs, clus_rel_lba(fs, c) + (coff >> 9),
                         sec);
            if (r)
                return r;
            memcpy(&sec[coff & 511], zeros,
                   chunk < 512 ? chunk : 512);
            /* partial-sector tail of the hole keeps old data? no:  */
            if (chunk < 512 && coff + chunk < 512)
                memset(&sec[(coff & 511) + chunk], 0,
                       512 - (coff & 511) - chunk);
            r = sec_write(fs, clus_rel_lba(fs, c) + (coff >> 9),
                          sec);
            if (r)
                return r;
            pos += chunk;
        }
    }

    cur.clus = in->first_clus;
    cur.idx = 0;

    while (done < len) {
        uint32_t want = (uint32_t)((off + done) / clus_bytes(fs));
        size_t coff = (off + done) % clus_bytes(fs);
        size_t chunk = clus_bytes(fs) - coff;
        uint32_t c;

        if (!cur.clus)
            return -EIO;
        r = chain_seek(fs, &cur, want, &c);
        if (r)
            return done ? (long)done : r;

        r = sec_read(fs, clus_rel_lba(fs, c) + (coff >> 9), sec);
        if (r)
            return done ? (long)done : r;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(&sec[coff & 511], (const uint8_t *)buf + done,
               chunk);
        r = sec_write(fs, clus_rel_lba(fs, c) + (coff >> 9), sec);
        if (r)
            return done ? (long)done : r;
        done += chunk;
    }

    if (off + len > in->size)
        in->size = off + len;
    r = dirent_sync(in);
    if (r)
        return r;
    return (long)done;
}

static long fat_write(struct vnode *vn, uint64_t off,
                     const void *buf, size_t len)
{
    struct fat_inode *in = vn->priv;
    int r;

    if (vn->type == V_DIR)
        return -EISDIR;

    /* vfs_open() O_TRUNC idiom: zero-length write at offset 0      */
    if (!len && off == 0 && !buf) {
        sem_acquire(&in->fs->sem);
        r = fat_truncate(in);
        sem_release(&in->fs->sem);
        return r;
    }

    if (!buf)
        return -EINVAL;

    sem_acquire(&in->fs->sem);
    r = fat_write_range(in, off, buf, len);
    sem_release(&in->fs->sem);
    return r;
}

static int fat_getattr(struct vnode *vn, struct vattr *out)
{
    struct fat_inode *in = vn->priv;

    out->type = in->is_dir ? V_DIR : V_FILE;
    out->size = in->is_dir ? 0 : in->size;
    return 0;
}

/* ---- mount / sniff / mkfs ---------------------------------------------------------------------------------------------------------------------- */

const struct vnode_ops fat_vops = {
    .lookup   = fat_lookup,
    .create   = fat_create,
    .unlink   = fat_unlink,
    .readdir  = fat_readdir,
    .read     = fat_read,
    .write    = fat_write,
    .getattr  = fat_getattr,
    .destroy  = fat_destroy,
};

static int fat_mount(struct mount *m)
{
    struct block_device *bd = m->bd;
    struct fat_fs *fs;
    struct fat_inode *root_in;
    struct vnode *root;
    uint8_t bs[512];
    uint32_t totsec, fatsz, data_start, clusters, spc, res, nfat;
    int r;

    if (!bd)
        return -ENODEV;
    r = block_read(bd, m->part_lba, bs, 512);
    if (r)
        return r;

    if (bs[510] != 0x55 || bs[511] != 0xAA)
        return -EINVAL;
    if (rd16(bs, 11) != 512)
        return -EINVAL;

    spc = bs[13];
    res = rd16(bs, 14);
    nfat = bs[16];
    if (!pow2(spc) || spc > 128 || res == 0 ||
        (nfat != 1 && nfat != 2))
        return -EINVAL;

    fatsz = rd32(bs, 36);
    if (!fatsz)
        fatsz = rd16(bs, 22);
    totsec = rd32(bs, 32);
    if (!totsec)
        totsec = rd16(bs, 19);
    data_start = res + nfat * fatsz;
    clusters = totsec > data_start
               ? (totsec - data_start) / spc : 0;

    if (fatsz == 0 || clusters < 4085 ||
        rd16(bs, 44) < 2 || rd16(bs, 44) >= clusters + 2)
        return -EINVAL;                 /* not a usable FAT32 volume  */

    fs = kzalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    fs->bd = bd;
    fs->part_lba = m->part_lba;
    fs->part_nsect = m->part_nsect;
    fs->sec_per_clus = spc;
    fs->reserved = res;
    fs->nfats = nfat;
    fs->fatsz = fatsz;
    fs->root_clus = rd16(bs, 44);
    fs->data_start = data_start;
    fs->total_clusters = clusters;
    fs->sem.lk = (spinlock_t)SPINLOCK_INIT;
    fs->sem.wq = (struct waitqueue){ NULL };

    root_in = kzalloc(sizeof(*root_in));
    if (!root_in) {
        kfree(fs);
        return -ENOMEM;
    }
    root_in->fs = fs;
    root_in->is_dir = true;
    root_in->first_clus = fs->root_clus;

    root = inode_shell(root_in);
    if (!root) {
        kfree(root_in);
        kfree(fs);
        return -ENOMEM;
    }
    root->mp = m;
    m->root = root;
    return 0;
}

void fat32_init(void)
{
    vfs_register_fs("vfat", fat_mount);
}

/* ---- probe + format ---------------------------------------------------------------------------------------------------------------- */

int fat32_sniff(struct block_device *bd, uint64_t lba, uint64_t nsect)
{
    uint8_t bs[512];
    uint32_t spc, res, nfat, fatsz, totsec, data_start, clusters;

    if (!bd || !nsect)
        return 0;
    if (block_read(bd, lba, bs, 512))
        return 0;
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        return 0;
    if (rd16(bs, 11) != 512)
        return 0;

    spc = bs[13];
    res = rd16(bs, 14);
    nfat = bs[16];
    fatsz = rd32(bs, 36);
    if (!fatsz)
        fatsz = rd16(bs, 22);
    totsec = rd32(bs, 32);
    if (!totsec)
        totsec = rd16(bs, 19);
    data_start = res + nfat * fatsz;
    if (!pow2(spc) || spc > 128 || res == 0 ||
        (nfat != 1 && nfat != 2) || fatsz == 0 ||
        totsec <= data_start || totsec > nsect)
        return 0;

    clusters = (totsec - data_start) / spc;
    return clusters >= 4085 && rd16(bs, 44) >= 2 &&
           rd16(bs, 44) < clusters + 2;
}

int fat32_mkfs(struct block_device *bd, uint64_t lba, uint64_t nsect)
{
    uint8_t sec[512];
    uint32_t reserved = 32;
    uint32_t fatsz = (uint32_t)((nsect - 30) / 130) + 2;
    uint32_t clusters = (uint32_t)(nsect - reserved - 2 * fatsz);
    uint64_t data_rel = reserved + 2 * fatsz;
    int r;

    if (clusters < 65525)
        return -EINVAL;                 /* would not be honest FAT32  */
    if (data_rel + clusters > nsect)
        return -EIO;

    memset(sec, 0, sizeof(sec));

    /* boot sector                                                  */
    sec[0] = 0xEB;
    sec[1] = 0x3C;
    sec[2] = 0x90;
    memcpy(&sec[3], "MPOSOS ", 8);
    wr16(sec, 11, 512);
    sec[13] = 1;                        /* sectors per cluster        */
    wr16(sec, 14, (uint16_t)reserved);
    sec[16] = 2;                        /* FAT copies                 */
    wr16(sec, 17, 0);                   /* root entries (FAT32: 0)    */
    wr16(sec, 19, 0);                   /* total16                    */
    sec[21] = 0xF8;                     /* media descriptor           */
    wr16(sec, 22, 0);                   /* fatsz16                    */
    wr16(sec, 24, 63);                  /* sectors per track          */
    wr16(sec, 26, 255);                 /* heads                      */
    wr32(sec, 28, 0);                   /* hidden sectors             */
    wr32(sec, 32, (uint32_t)nsect);
    wr32(sec, 36, fatsz);
    wr32(sec, 40, 0);                   /* drive flags                */
    wr16(sec, 44, 2);                   /* root dir first cluster     */
    wr16(sec, 46, 1);                   /* FSInfo sector              */
    wr16(sec, 48, 6);                   /* backup boot sector         */
    sec[64] = 0x80;                     /* drive number               */
    sec[65] = 0x29;                     /* extended boot signature    */
    wr32(sec, 67, 0x4D504F53);          /* volume id                  */
    memcpy(&sec[71], "MPOSOSDISK ", 11);
    memcpy(&sec[82], "FAT32   ", 8);
    sec[510] = 0x55;
    sec[511] = 0xAA;

    r = block_write(bd, lba, sec, 512);
    if (r)
        return r;
    r = block_write(bd, lba + 6, sec, 512);     /* backup boot        */
    if (r)
        return r;

    /* FSInfo (sector 1) + backup (7)                               */
    memset(sec, 0, sizeof(sec));
    wr32(sec, 0, 0x41615252);
    wr32(sec, 484, 0x61417272);
    wr32(sec, 488, 0xFFFFFFFF);         /* unknown free count         */
    wr32(sec, 492, 0xFFFFFFFF);
    wr32(sec, 508, 0xAA550000);
    r = block_write(bd, lba + 1, sec, 512);
    if (r)
        return r;
    r = block_write(bd, lba + 7, sec, 512);
    if (r)
        return r;

    /* FAT copies: reserved entries + EOC for the root cluster      */
    for (uint32_t copy = 0; copy < 2; copy++) {
        for (uint32_t s = 0; s < fatsz; s++) {
            uint8_t fsec[512];

            memset(fsec, 0, sizeof(fsec));
            if (s == 0) {
                wr32(fsec, 0, 0x0FFFFFF8);  /* media descriptor       */
                wr32(fsec, 4, 0x0FFFFFFF);  /* reserved               */
                wr32(fsec, 8, 0x0FFFFFFF);  /* FAT[2]: root dir, EOC  */
            }
            r = block_write(bd, lba + reserved +
                            (uint64_t)copy * fatsz + s,
                            fsec, 512);
            if (r)
                return r;
        }
    }

    /* zero the single-cluster root directory                       */
    memset(sec, 0, sizeof(sec));
    return block_write(bd, lba + data_rel, sec, 512);
}
