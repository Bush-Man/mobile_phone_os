/*
 * ext2.c - ext2-lite filesystem with read AND write support
 * (phase 7).
 *
 * Geometry assumptions (what our mkfs emits, and what every small
 * mke2fs volume uses):
 *   - 1024-byte blocks (log_block_size 0), two sectors per block,
 *   - revision-1 superblock, 128-byte inodes, first_ino = 11,
 *   - first_data_block = 1 (the superblock is group 0's block 1),
 *   - blocks_per_group = 8192, so one bitmap block covers a group,
 *   - direct + singly/doubly/triply indirect block pointers.
 *
 * Journaling is deliberately absent: the driver neither replays nor
 * writes journal metadata, and mkfs reserves none.
 *
 * Locking mirrors fs/fat32.c: block IO sleeps, so every mounted
 * instance carries a sleeping big-fs-lock (flag + phase-4 wait
 * queue) taken by each vnode operation; internal helpers assume it
 * is held and never re-acquire.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "block.h"
#include "ext2.h"
#include "lib.h"
#include "mm/kheap.h"
#include "panic.h"
#include "sync.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"

#define E2_BSIZE        1024u
#define E2_SECT_PER_BLK 2u              /* BLK_SECTOR_SIZE ratio      */
#define E2_MAGIC        0xEF53u
#define E2_BPG          8192u           /* blocks per group           */
#define E2_IPG          2048u           /* inodes per group           */
#define E2_INODE_SIZE   128u
#define E2_FIRST_INO    11u
#define E2_ROOT_INO     2u
#define E2_NGROUP_MAX   16u

#define E2_M_DIR   0x4000u
#define E2_M_PERM  0755u

#define E2_FT_REG  1u
#define E2_FT_DIR  2u

#define E2_MAX_FILE (16u * 1024u * 1024u)     /* sanity cap          */

/* errno extension (Linux value) */
#ifndef EFBIG
#define EFBIG 27
#endif

const struct vnode_ops ext2_vops;

/*
 * The sleeping big-fs-lock (phase 8): the hand-rolled flag+waitqueue
 * duplicate from phase 7 is gone -- kernel/sync.c now provides a real
 * blocking mutex with owner tracking. Call sites were untouched; only
 * the primitive underneath them changed (see fs/fat32.c, same shape).
 */
static void sem_acquire(struct kmutex *sem)
{
    if (kmutex_lock(sem))
        panic("ext2: filesystem lock would deadlock");
}

static void sem_release(struct kmutex *sem)
{
    kmutex_unlock(sem);
}

/* ---- core types ---------------------------------------------------------------------- */

struct e2_grp {
    uint32_t bbmap;                     /* block bitmap abs block     */
    uint32_t ibmap;                     /* inode bitmap               */
    uint32_t itable;                    /* first inode-table block    */
};

struct e2_fs {
    struct block_device *bd;
    uint64_t part_lba;
    uint64_t part_nsect;

    uint32_t blocks_count;              /* includes boot block 0      */
    uint32_t ngroups;
    struct e2_grp grp[E2_NGROUP_MAX];

    struct kmutex sem;
};

struct e2_inode {
    struct e2_fs *fs;
    uint32_t ino;
    bool is_dir;
    uint64_t size;
    uint16_t links;
    uint32_t blk[15];                   /* direct + indirect roots    */
};

struct e2_dirent {
    uint32_t ino;
    uint8_t ftype;
    char name[256];
};

/* ---- little-endian -------------------------------------------------------------------- */

static inline uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

static inline uint32_t le32(const uint8_t *b)
{
    return b[0] | (b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline void st16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static inline void st32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

/* ---- block IO ------------------------------------------------------------------------------ */

static int blk_read(const struct e2_fs *fs, uint32_t ab, void *buf)
{
    if ((uint64_t)ab * E2_SECT_PER_BLK >= fs->part_nsect)
        return -EIO;
    return block_read(fs->bd,
                      fs->part_lba + (uint64_t)ab * E2_SECT_PER_BLK,
                      buf, E2_BSIZE);
}

static int blk_write(const struct e2_fs *fs, uint32_t ab,
                     const void *buf)
{
    if ((uint64_t)ab * E2_SECT_PER_BLK >= fs->part_nsect)
        return -EIO;
    return block_write(fs->bd,
                       fs->part_lba + (uint64_t)ab * E2_SECT_PER_BLK,
                       buf, E2_BSIZE);
}

static int sect_rw(const struct e2_fs *fs, uint64_t sect, void *buf,
                   unsigned nsect, bool write)
{
    if (sect + nsect > fs->part_nsect)
        return -EIO;
    return write
        ? block_write(fs->bd, fs->part_lba + sect, buf,
                      nsect * BLK_SECTOR_SIZE)
        : block_read(fs->bd, fs->part_lba + sect, buf,
                     nsect * BLK_SECTOR_SIZE);
}

/* ---- inode persistence ---------------------------------------------------------------------------- */

/*
 * Inode records straddle sector boundaries freely (128-byte stride
 * through a contiguous table), so gather/scatter through staging.
 */
static int inode_gather(const struct e2_fs *fs, uint32_t ino,
                        uint8_t *raw /* E2_INODE_SIZE */)
{
    uint32_t g = (ino - 1) / E2_IPG, idx = (ino - 1) % E2_IPG;
    uint64_t boff = (uint64_t)fs->grp[g].itable * E2_BSIZE +
                    (uint64_t)idx * E2_INODE_SIZE;
    unsigned off = boff & 511u;
    uint8_t sec[512];
    int r;

    r = sect_rw(fs, boff >> 9, sec, 1, false);
    if (r)
        return r;
    if (off + E2_INODE_SIZE <= 512) {
        memcpy(raw, &sec[off], E2_INODE_SIZE);
        return 0;
    }
    {
        unsigned head = 512u - off;

        memcpy(raw, &sec[off], head);
        r = sect_rw(fs, (boff >> 9) + 1, sec, 1, false);
        if (r)
            return r;
        memcpy(&raw[head], sec, E2_INODE_SIZE - head);
    }
    return 0;
}

static int inode_scatter(const struct e2_fs *fs, uint32_t ino,
                         const uint8_t *raw)
{
    uint32_t g = (ino - 1) / E2_IPG, idx = (ino - 1) % E2_IPG;
    uint64_t boff = (uint64_t)fs->grp[g].itable * E2_BSIZE +
                    (uint64_t)idx * E2_INODE_SIZE;
    unsigned off = boff & 511u;
    uint8_t sec[512];
    int r;

    r = sect_rw(fs, boff >> 9, sec, 1, false);
    if (r)
        return r;
    if (off + E2_INODE_SIZE <= 512) {
        memcpy(&sec[off], raw, E2_INODE_SIZE);
        return sect_rw(fs, boff >> 9, sec, 1, true);
    }
    {
        unsigned head = 512u - off;
        int r2;

        memcpy(&sec[off], raw, head);
        r = sect_rw(fs, boff >> 9, sec, 1, true);
        if (r)
            return r;
        r2 = sect_rw(fs, (boff >> 9) + 1, sec, 1, false);
        if (r2)
            return r2;
        memcpy(sec, &raw[head], E2_INODE_SIZE - head);
        return sect_rw(fs, (boff >> 9) + 1, sec, 1, true);
    }
}

static int inode_load(struct e2_fs *fs, uint32_t ino,
                      struct e2_inode *out)
{
    uint8_t raw[E2_INODE_SIZE];
    uint16_t mode;
    int r;

    r = inode_gather(fs, ino, raw);
    if (r)
        return r;

    memset(out, 0, sizeof(*out));
    out->fs = fs;
    out->ino = ino;
    mode = le16(&raw[0]);
    out->is_dir = (mode & 0xF000u) == E2_M_DIR;
    out->size = le32(&raw[4]);
    out->links = le16(&raw[26]);
    for (int i = 0; i < 15; i++)
        out->blk[i] = le32(&raw[40 + i * 4]);
    return 0;
}

static int inode_store(struct e2_inode *in)
{
    uint8_t raw[E2_INODE_SIZE];
    uint16_t mode = in->is_dir ? (E2_M_DIR | E2_M_PERM)
                               : (0x8000u | E2_M_PERM);

    memset(raw, 0, sizeof(raw));
    st16(&raw[0], mode);
    st32(&raw[4], (uint32_t)in->size);
    st16(&raw[26], in->links);
    st32(&raw[28],
         (uint32_t)((in->size + E2_BSIZE - 1) / E2_BSIZE) *
         E2_SECT_PER_BLK);
    for (int i = 0; i < 15; i++)
        st32(&raw[40 + i * 4], in->blk[i]);
    return inode_scatter(in->fs, in->ino, raw);
}

/* ---- bitmaps ------------------------------------------------------------------------------------------------------ */

/* caller holds sem                                                */
static int balloc(struct e2_fs *fs, uint32_t *out)
{
    uint8_t bm[E2_BSIZE];
    int r;

    for (uint32_t g = 0; g < fs->ngroups; g++) {
        r = blk_read(fs, fs->grp[g].bbmap, bm);
        if (r)
            return r;
        for (uint32_t rel = 0; rel < E2_BPG; rel++) {
            uint32_t ab = g * E2_BPG + 1u + rel;

            if (ab >= fs->blocks_count)
                break;                  /* beyond volume end         */
            if (!(bm[rel >> 3] & (1u << (rel & 7)))) {
                bm[rel >> 3] |= (uint8_t)(1u << (rel & 7));
                r = blk_write(fs, fs->grp[g].bbmap, bm);
                if (r)
                    return r;
                *out = ab;
                return 0;
            }
        }
    }
    return -ENOSPC;
}

/* allocate + zero-fill (fits both data and metadata blocks)       */
static int balloc_zero(struct e2_fs *fs, uint32_t *out)
{
    static uint8_t zero[E2_BSIZE];
    int r = balloc(fs, out);

    if (r)
        return r;
    return blk_write(fs, *out, zero);
}

static int bfree(struct e2_fs *fs, uint32_t ab)
{
    uint32_t g, rel;
    uint8_t bm[E2_BSIZE];
    int r;

    if (!ab || ab >= fs->blocks_count)
        return -EIO;
    g = (ab - 1) / E2_BPG;
    rel = (ab - 1) % E2_BPG;

    r = blk_read(fs, fs->grp[g].bbmap, bm);
    if (r)
        return r;
    if (!(bm[rel >> 3] & (1u << (rel & 7))))
        return -EIO;                    /* double free               */
    bm[rel >> 3] &= (uint8_t)~(1u << (rel & 7));
    return blk_write(fs, fs->grp[g].bbmap, bm);
}

/* caller holds sem; reserved inodes 1..10 stay taken forever      */
static int ialloc(struct e2_fs *fs, uint32_t *out)
{
    uint8_t bm[E2_BSIZE];
    int r;

    for (uint32_t g = 0; g < fs->ngroups; g++) {
        uint32_t start_rel = g == 0 ? E2_FIRST_INO - 1 : 0;

        r = blk_read(fs, fs->grp[g].ibmap, bm);
        if (r)
            return r;
        for (uint32_t rel = start_rel; rel < E2_IPG; rel++) {
            if (!(bm[rel >> 3] & (1u << (rel & 7)))) {
                uint32_t ino = g * E2_IPG + rel + 1;

                bm[rel >> 3] |= (uint8_t)(1u << (rel & 7));
                r = blk_write(fs, fs->grp[g].ibmap, bm);
                if (r)
                    return r;
                *out = ino;
                return 0;
            }
        }
    }
    return -ENOSPC;
}

static int ifree(struct e2_fs *fs, uint32_t ino)
{
    uint32_t g = (ino - 1) / E2_IPG, rel = (ino - 1) % E2_IPG;
    uint8_t bm[E2_BSIZE];
    int r;

    if (ino <= E2_FIRST_INO || g >= fs->ngroups)
        return -EINVAL;
    r = blk_read(fs, fs->grp[g].ibmap, bm);
    if (r)
        return r;
    bm[rel >> 3] &= (uint8_t)~(1u << (rel & 7));
    return blk_write(fs, fs->grp[g].ibmap, bm);
}

/* ---- logical -> physical mapping ------------------------------------------------------------------------------------------ */

/*
 * Recursive walker: the subtree rooted at *rootp covers
 * P^level logical blocks; level 1 means *rootp IS a data block.
 * Freshly allocated roots are reported through *changed so whoever
 * owns *rootp (an indirect-block buffer or the in-memory inode)
 * persists the patched pointer.
 */
static int bmap_level(struct e2_fs *fs, uint32_t *rootp,
                      unsigned level, uint32_t idx, bool alloc,
                      bool *changed, uint32_t *out)
{
    const uint32_t P = E2_BSIZE / 4u;
    uint32_t stride = 1, slot;
    uint8_t ib[E2_BSIZE];
    int r;

    for (unsigned k = 1; k < level; k++)
        stride *= P;
    slot = idx / stride;

    if (!*rootp) {
        if (!alloc) {
            *out = 0;
            return 0;
        }
        r = balloc_zero(fs, rootp);     /* leaf or metadata block    */
        if (r)
            return r;
        *changed = true;
    }

    if (level == 1) {
        *out = *rootp;
        return 0;
    }

    r = blk_read(fs, *rootp, ib);
    if (r)
        return r;

    {
        bool child_changed = false;

        r = bmap_level(fs, (uint32_t *)&ib[slot * 4], level - 1,
                       idx % stride, alloc, &child_changed, out);
        if (r)
            return r;
        if (child_changed) {
            r = blk_write(fs, *rootp, ib);
            if (r)
                return r;
        }
    }
    return 0;
}

/* logical file block -> absolute block (0 = hole); sem held       */
static int bmap(struct e2_fs *fs, struct e2_inode *in, uint32_t fb,
                bool alloc, uint32_t *out)
{
    const uint32_t P = E2_BSIZE / 4u;
    uint32_t *rootp;
    unsigned level;
    uint32_t idx = 0;
    bool changed = false;
    int r;

    if ((uint64_t)(fb + 1) * E2_BSIZE > E2_MAX_FILE)
        return -EFBIG;

    if (fb < 12) {
        rootp = &in->blk[fb];
        level = 1;
    } else if (fb < 12 + P) {
        rootp = &in->blk[12];
        level = 2;
        idx = fb - 12;
    } else if (fb < 12 + P + P * P) {
        rootp = &in->blk[13];
        level = 3;
        idx = fb - 12 - P;
    } else {
        rootp = &in->blk[14];
        level = 4;
        idx = fb - 12 - P - P * P;
    }

    r = bmap_level(fs, rootp, level, idx, alloc, &changed, out);
    if (r)
        return r;
    if (changed)
        return inode_store(in);         /* persist patched root      */
    return 0;
}

/* free every block (data + indirect metadata) below one pointer   */
static int free_level(struct e2_fs *fs, uint32_t root, unsigned level)
{
    const uint32_t P = E2_BSIZE / 4u;
    uint8_t ib[E2_BSIZE];
    int r;

    if (!root)
        return 0;
    if (level == 1)
        return bfree(fs, root);

    r = blk_read(fs, root, ib);
    if (r)
        return r;
    for (uint32_t s = 0; s < P; s++) {
        uint32_t child = le32(&ib[s * 4]);

        if (child) {
            r = free_level(fs, child, level - 1);
            if (r)
                return r;
        }
    }
    return bfree(fs, root);
}

static int release_all_blocks(struct e2_inode *in)
{
    struct e2_fs *fs = in->fs;
    int r;

    for (int i = 0; i < 12; i++) {
        if (in->blk[i]) {
            r = bfree(fs, in->blk[i]);
            if (r)
                return r;
            in->blk[i] = 0;
        }
    }
    if (in->blk[12]) {
        r = free_level(fs, in->blk[12], 2);
        if (r)
            return r;
        in->blk[12] = 0;
    }
    if (in->blk[13]) {
        r = free_level(fs, in->blk[13], 3);
        if (r)
            return r;
        in->blk[13] = 0;
    }
    if (in->blk[14]) {
        r = free_level(fs, in->blk[14], 4);
        if (r)
            return r;
        in->blk[14] = 0;
    }
    in->size = 0;
    return inode_store(in);
}

/* ---- directories ------------------------------------------------------------------------------------------------------------------ */

/* read the record header (+ body) at absolute byte offset doff    */
static int dir_ent_at(struct e2_fs *fs, struct e2_inode *dir,
                      uint64_t doff, uint16_t *rec_len,
                      struct e2_dirent *out /* optional */)
{
    uint32_t in_block = (uint32_t)(doff % E2_BSIZE);
    uint32_t ab;
    uint8_t bb[E2_BSIZE];
    int r;

    r = bmap(fs, dir, (uint32_t)(doff / E2_BSIZE), false, &ab);
    if (r)
        return r;
    if (!ab)
        return -EIO;                    /* holes are illegal in dirs */
    r = blk_read(fs, ab, bb);
    if (r)
        return r;

    *rec_len = le16(&bb[in_block + 4]);
    if (*rec_len < 8 || (in_block & 3) ||
        in_block + *rec_len > E2_BSIZE)
        return -EIO;

    if (out) {
        out->ino = le32(&bb[in_block]);
        out->ftype = bb[in_block + 7];
        {
            uint8_t nl = bb[in_block + 6];

            memcpy(out->name, &bb[in_block + 8], nl);
            out->name[nl] = '\0';
        }
    }
    return 0;
}

/* patch just the record length at doff                            */
static int dir_patch_reclen(struct e2_fs *fs, struct e2_inode *dir,
                            uint64_t doff, uint16_t rec_len)
{
    uint32_t in_block = (uint32_t)(doff % E2_BSIZE);
    uint32_t ab;
    uint8_t bb[E2_BSIZE];
    int r;

    r = bmap(fs, dir, (uint32_t)(doff / E2_BSIZE), false, &ab);
    if (r)
        return r;
    if (!ab)
        return -EIO;
    r = blk_read(fs, ab, bb);
    if (r)
        return r;
    st16(&bb[in_block + 4], rec_len);
    return blk_write(fs, ab, bb);
}

/* overwrite the record at doff with a live entry                  */
static int dir_put_entry(struct e2_fs *fs, struct e2_inode *dir,
                         uint64_t doff, uint32_t child_ino,
                         const char *name, uint8_t ftype)
{
    uint32_t in_block = (uint32_t)(doff % E2_BSIZE);
    uint32_t ab;
    uint8_t bb[E2_BSIZE];
    size_t nlen = strlen(name);
    int r;

    if (8 + nlen > E2_BSIZE)
        return -ENAMETOOLONG;
    r = bmap(fs, dir, (uint32_t)(doff / E2_BSIZE), false, &ab);
    if (r)
        return r;
    if (!ab)
        return -EIO;
    r = blk_read(fs, ab, bb);
    if (r)
        return r;
    st32(&bb[in_block], child_ino);
    bb[in_block + 6] = (uint8_t)nlen;
    bb[in_block + 7] = ftype;
    memcpy(&bb[in_block + 8], name, nlen);
    return blk_write(fs, ab, bb);
}

/* find name -> absolute byte offset + dirent fill; 1 hit/0 miss   */
static int dir_find(struct e2_fs *fs, struct e2_inode *dir,
                    const char *name, uint64_t *doff_out,
                    struct e2_dirent *ent_out)
{
    uint64_t doff = 0;

    while (doff < dir->size) {
        uint16_t rl;
        struct e2_dirent ent;
        int r = dir_ent_at(fs, dir, doff, &rl, &ent);

        if (r)
            return r;
        if (ent.ino && strcmp(ent.name, name) == 0) {
            *doff_out = doff;
            *ent_out = ent;
            return 1;
        }
        doff += rl;
    }
    return 0;
}

/* caller holds sem                                                */
static int dir_add_entry(struct e2_fs *fs, struct e2_inode *dir,
                         uint32_t child_ino, const char *name,
                         uint8_t ftype)
{
    size_t nlen = strlen(name);
    uint32_t need = ((uint32_t)(8 + nlen) + 3u) & ~3u;
    uint32_t nblocks = (uint32_t)(dir->size / E2_BSIZE);
    int r;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t ab;
        uint8_t bb[E2_BSIZE];
        uint32_t off = 0;

        r = bmap(fs, dir, bi, false, &ab);
        if (r)
            return r;
        if (!ab)
            continue;
        r = blk_read(fs, ab, bb);
        if (r)
            return r;

        while (off < E2_BSIZE) {
            uint16_t rl = le16(&bb[off + 4]);
            uint8_t nl = bb[off + 6];
            uint32_t min_len = ((uint32_t)(8 + nl) + 3u) & ~3u;

            if (rl < 8 || off + rl > E2_BSIZE)
                return -EIO;

            if (le32(&bb[off]) != 0) {
                /*
                 * Live record with slack: shrink it and splice the
                 * new entry into the freed tail of the same slot.
                 */
                if (rl >= min_len + need) {
                    uint32_t noff = off + min_len;

                    st16(&bb[off], (uint16_t)min_len);
                    st32(&bb[noff], child_ino);
                    st16(&bb[noff + 4], (uint16_t)(rl - min_len));
                    bb[noff + 6] = (uint8_t)nlen;
                    bb[noff + 7] = ftype;
                    memcpy(&bb[noff + 8], name, nlen);
                    return blk_write(fs, ab, bb);
                }
            } else if (rl >= need) {
                /*
                 * Free-standing tombstone spanning enough space:
                 * reuse it wholesale (keeps its full length).
                 */
                st32(&bb[off], child_ino);
                bb[off + 6] = (uint8_t)nlen;
                bb[off + 7] = ftype;
                memcpy(&bb[off + 8], name, nlen);
                return blk_write(fs, ab, bb);
            }
            off += rl;
        }
    }

    /* no room anywhere: append a fresh directory block             */
    {
        uint32_t ab;
        uint8_t bb[E2_BSIZE];

        r = bmap(fs, dir, nblocks, true, &ab);
        if (r)
            return r;

        memset(bb, 0, sizeof(bb));
        st32(&bb[0], child_ino);
        st16(&bb[4], E2_BSIZE);
        bb[6] = (uint8_t)nlen;
        bb[7] = ftype;
        memcpy(&bb[8], name, nlen);
        r = blk_write(fs, ab, bb);
        if (r)
            return r;

        dir->size += E2_BSIZE;
        return inode_store(dir);
    }
}

/*
 * Unlink the record at doff. Standard scheme: merge its length into
 * the PREVIOUS record; when it opens its block, leave an explicit
 * tombstone (ino = 0, full length kept) that dir_add_entry reuses.
 */
static int dir_remove_entry(struct e2_fs *fs, struct e2_inode *dir,
                            uint64_t doff)
{
    uint32_t bi = (uint32_t)(doff / E2_BSIZE);
    uint32_t in_block = (uint32_t)(doff % E2_BSIZE);
    uint32_t ab;
    uint8_t bb[E2_BSIZE];
    uint32_t off = 0;
    uint16_t victim_rl;
    int r;

    r = bmap(fs, dir, bi, false, &ab);
    if (r)
        return r;
    if (!ab)
        return -EIO;
    r = blk_read(fs, ab, bb);
    if (r)
        return r;

    victim_rl = le16(&bb[in_block + 4]);

    if (in_block == 0) {
        st32(&bb[0], 0);                /* tombstone, length kept    */
        return blk_write(fs, ab, bb);
    }

    while (off < in_block) {
        uint16_t rl = le16(&bb[off + 4]);

        if (rl < 8 || off + rl > E2_BSIZE)
            return -EIO;
        if (off + rl == in_block) {
            st16(&bb[off], (uint16_t)(rl + victim_rl));
            return blk_write(fs, ab, bb);
        }
        off += rl;
    }
    return -EIO;                        /* chain lost the boundary   */
}

static bool dir_is_empty(struct e2_fs *fs, struct e2_inode *dir)
{
    uint64_t doff = 0;

    while (doff < dir->size) {
        uint16_t rl;
        struct e2_dirent ent;
        int r = dir_ent_at(fs, dir, doff, &rl, &ent);

        if (r)
            return true;                /* unreadable: treat as such */
        if (ent.ino && strcmp(ent.name, ".") != 0 &&
            strcmp(ent.name, "..") != 0)
            return false;
        doff += rl;
    }
    return true;
}

/* ---- vnode shells -------------------------------------------------------------------------------------------------------------------- */

static struct vnode *shell_from(struct e2_inode *in)
{
    struct vnode *vn = kzalloc(sizeof(*vn));

    if (!vn)
        return NULL;
    vn->ops = &ext2_vops;
    vn->type = in->is_dir ? V_DIR : V_FILE;
    vn->priv = in;
    vn->ino = in->ino;
    vn->refs = 1;                       /* caller (or mount) owns it  */
    return vn;
}

static void ext2_destroy(struct vnode *vn)
{
    kfree(vn->priv);
    kfree(vn);
}

/* ---- vnode ops ---------------------------------------------------------------------------------------------------------------------------- */

static int ext2_lookup(struct vnode *dir, const char *name,
                       struct vnode **out)
{
    struct e2_inode *d = dir->priv;
    struct e2_fs *fs = d->fs;
    struct e2_inode *in;
    struct e2_dirent ent;
    uint64_t doff;
    int r;

    if (!name[0])
        return -ENOENT;

    sem_acquire(&fs->sem);

    r = dir_find(fs, d, name, &doff, &ent);
    if (r <= 0) {
        sem_release(&fs->sem);
        return r == 0 ? -ENOENT : r;
    }

    in = kzalloc(sizeof(*in));
    if (!in) {
        sem_release(&fs->sem);
        return -ENOMEM;
    }
    r = inode_load(fs, ent.ino, in);
    if (r) {
        kfree(in);
        sem_release(&fs->sem);
        return r;
    }

    *out = shell_from(in);
    sem_release(&fs->sem);
    if (!*out) {
        kfree(in);
        return -ENOMEM;
    }
    return 0;
}

static int ext2_readdir(struct vnode *dir, unsigned idx,
                        char *name_out, uint8_t *type_out)
{
    struct e2_inode *d = dir->priv;
    struct e2_fs *fs = d->fs;
    uint64_t doff = 0;
    unsigned seen = 0;
    int r;

    sem_acquire(&fs->sem);

    while (doff < d->size) {
        uint16_t rl;
        struct e2_dirent ent;

        r = dir_ent_at(fs, d, doff, &rl, &ent);
        if (r) {
            sem_release(&fs->sem);
            return r;
        }
        doff += rl;

        if (!ent.ino)
            continue;
        if (strcmp(ent.name, ".") == 0 ||
            strcmp(ent.name, "..") == 0)
            continue;
        if (seen++ == idx) {
            kstrlcpy(name_out, ent.name, VFS_NAME_MAX);
            *type_out = ent.ftype == E2_FT_DIR ? DT_DIR
                                               : DT_REG;
            sem_release(&fs->sem);
            return 0;
        }
    }

    sem_release(&fs->sem);
    return -ENOENT;
}

static int ext2_mkdir_child(struct e2_fs *fs, struct e2_inode *parent,
                            const char *name, struct e2_inode **out);

static int ext2_create(struct vnode *dir, const char *name,
                       enum vtype t, struct vnode **out)
{
    struct e2_inode *d = dir->priv;
    struct e2_fs *fs = d->fs;
    struct e2_inode *fresh = NULL;
    struct vnode *vn;
    int r;

    if (strlen(name) >= 255)
        return -ENAMETOOLONG;

    sem_acquire(&fs->sem);

    if (t == V_DIR) {
        r = ext2_mkdir_child(fs, d, name, &fresh);
        if (r) {
            sem_release(&fs->sem);
            return r;
        }
    } else {
        uint32_t ino;
        struct e2_dirent ent;
        uint64_t doff;

        r = dir_find(fs, d, name, &doff, &ent);
        if (r > 0) {
            sem_release(&fs->sem);
            return -EEXIST;
        }
        if (r < 0) {
            sem_release(&fs->sem);
            return r;
        }

        r = ialloc(fs, &ino);
        if (r) {
            sem_release(&fs->sem);
            return r;
        }

        fresh = kzalloc(sizeof(*fresh));
        if (!fresh) {
            ifree(fs, ino);
            sem_release(&fs->sem);
            return -ENOMEM;
        }
        fresh->fs = fs;
        fresh->ino = ino;
        fresh->links = 1;

        r = inode_store(fresh);
        if (r)
            goto fail;

        r = dir_add_entry(fs, d, ino, name, E2_FT_REG);
        if (r)
            goto fail;
    }

    vn = shell_from(fresh);
    if (!vn) {
        r = -ENOMEM;
        goto fail;
    }
    sem_release(&fs->sem);
    *out = vn;
    return 0;

fail:
    if (fresh) {
        if (fresh->size || fresh->blk[0])
            release_all_blocks(fresh);
        ifree(fs, fresh->ino);
        kfree(fresh);
    }
    sem_release(&fs->sem);
    return r;
}

/* shared tail of create(): dot entries, parent link, bookkeeping  */
static int ext2_mkdir_child(struct e2_fs *fs, struct e2_inode *parent,
                            const char *name, struct e2_inode **out)
{
    struct e2_dirent ent;
    uint64_t doff;
    uint32_t ino, ab;
    struct e2_inode *fresh;
    uint8_t bb[E2_BSIZE];
    int r;

    r = dir_find(fs, parent, name, &doff, &ent);
    if (r > 0)
        return -EEXIST;
    if (r < 0)
        return r;

    r = ialloc(fs, &ino);
    if (r)
        return r;

    fresh = kzalloc(sizeof(*fresh));
    if (!fresh) {
        ifree(fs, ino);
        return -ENOMEM;
    }
    fresh->fs = fs;
    fresh->ino = ino;
    fresh->is_dir = true;
    fresh->links = 2;                   /* ".", plus parent's entry   */

    r = bmap(fs, fresh, 0, true, &ab);  /* zero-filled first block    */
    if (r)
        goto fail;

    /* ".", ".."                                                    */
    r = blk_read(fs, ab, bb);
    if (r)
        goto fail;
    memset(bb, 0, sizeof(bb));
    st32(&bb[0], ino);
    st16(&bb[4], 12);
    bb[6] = 1;
    bb[7] = E2_FT_DIR;
    memcpy(&bb[8], ".", 1);
    st32(&bb[12], ino);
    st16(&bb[16], E2_BSIZE - 12u);
    bb[22] = 2;
    bb[23] = E2_FT_DIR;
    memcpy(&bb[24], "..", 2);
    r = blk_write(fs, ab, bb);
    if (r)
        goto fail;
    fresh->size = E2_BSIZE;

    r = inode_store(fresh);
    if (r)
        goto fail;

    r = dir_add_entry(fs, parent, ino, name, E2_FT_DIR);
    if (r)
        goto fail;

    parent->links++;                    /* child's ".."               */
    r = inode_store(parent);
    if (r)
        goto fail;

    *out = fresh;
    return 0;

fail:
    if (fresh->size || fresh->blk[0])
        release_all_blocks(fresh);
    ifree(fs, ino);
    kfree(fresh);
    return r;
}

static int ext2_unlink(struct vnode *dir, const char *name)
{
    struct e2_inode *d = dir->priv;
    struct e2_fs *fs = d->fs;
    struct e2_dirent ent;
    struct e2_inode victim;
    uint64_t doff;
    bool was_dir;
    int r;

    sem_acquire(&fs->sem);

    r = dir_find(fs, d, name, &doff, &ent);
    if (r <= 0) {
        sem_release(&fs->sem);
        return r == 0 ? -ENOENT : r;
    }
    if (!strcmp(ent.name, ".") || !strcmp(ent.name, "..")) {
        sem_release(&fs->sem);
        return -EINVAL;
    }

    r = inode_load(fs, ent.ino, &victim);
    if (r) {
        sem_release(&fs->sem);
        return r;
    }
    was_dir = victim.is_dir;

    if (was_dir && !dir_is_empty(fs, &victim)) {
        sem_release(&fs->sem);
        return -ENOTEMPTY;
    }

    r = release_all_blocks(&victim);
    if (r) {
        sem_release(&fs->sem);
        return r;
    }
    r = ifree(fs, ent.ino);
    if (r) {
        sem_release(&fs->sem);
        return r;
    }
    r = dir_remove_entry(fs, d, doff);
    if (r) {
        sem_release(&fs->sem);
        return r;
    }

    if (was_dir) {
        d->links--;
        r = inode_store(d);
        if (r) {
            sem_release(&fs->sem);
            return r;
        }
    }

    sem_release(&fs->sem);
    return 0;
}

/* ---- data ------------------------------------------------------------------------------------------------------------------------------- */

static long ext2_read(struct vnode *vn, uint64_t off,
                     void *buf, size_t len)
{
    struct e2_inode *in = vn->priv;
    struct e2_fs *fs = in->fs;
    uint8_t bb[E2_BSIZE];
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

    while (done < len) {
        uint32_t fb = (uint32_t)((off + done) / E2_BSIZE);
        uint32_t coff = (uint32_t)((off + done) % E2_BSIZE);
        uint32_t chunk = E2_BSIZE - coff, ab;

        if (chunk > len - done)
            chunk = (uint32_t)(len - done);

        r = bmap(fs, in, fb, false, &ab);
        if (r) {
            sem_release(&fs->sem);
            return done ? (long)done : r;
        }
        if (!ab) {
            memset((uint8_t *)buf + done, 0, chunk);   /* hole     */
        } else {
            r = blk_read(fs, ab, bb);
            if (r) {
                sem_release(&fs->sem);
                return done ? (long)done : r;
            }
            memcpy((uint8_t *)buf + done, &bb[coff], chunk);
        }
        done += chunk;
    }

    sem_release(&fs->sem);
    return (long)done;
}

/* truncate to zero: free every mapped block                       */
static int ext2_truncate(struct e2_inode *in)
{
    return release_all_blocks(in);
}

static long ext2_write(struct vnode *vn, uint64_t off,
                      const void *buf, size_t len)
{
    struct e2_inode *in = vn->priv;
    struct e2_fs *fs = in->fs;
    uint8_t bb[E2_BSIZE];
    size_t done = 0;
    int r;

    if (vn->type == V_DIR)
        return -EISDIR;

    /* vfs_open() O_TRUNC idiom: zero-length write at offset 0      */
    if (!len && off == 0 && !buf) {
        sem_acquire(&fs->sem);
        r = ext2_truncate(in);
        sem_release(&fs->sem);
        return r;
    }
    if (!buf)
        return -EINVAL;

    sem_acquire(&fs->sem);

    if (off + len > E2_MAX_FILE) {
        sem_release(&fs->sem);
        return -EFBIG;
    }

    while (done < len) {
        uint32_t fb = (uint32_t)((off + done) / E2_BSIZE);
        uint32_t coff = (uint32_t)((off + done) % E2_BSIZE);
        uint32_t chunk = E2_BSIZE - coff, ab;
        bool partial_head = coff != 0;
        bool partial_tail = chunk > len - done;

        if (chunk > len - done)
            chunk = (uint32_t)(len - done);

        if (partial_head || partial_tail) {
            r = bmap(fs, in, fb, true, &ab);
            if (!r && ab)
                r = blk_read(fs, ab, bb);
            if (r) {
                sem_release(&fs->sem);
                return done ? (long)done : r;
            }
            memcpy(&bb[coff], (const uint8_t *)buf + done, chunk);
            r = blk_write(fs, ab, bb);
            if (r) {
                sem_release(&fs->sem);
                return done ? (long)done : r;
            }
        } else {
            r = bmap(fs, in, fb, true, &ab);
            if (r) {
                sem_release(&fs->sem);
                return done ? (long)done : r;
            }
            memcpy(bb, (const uint8_t *)buf + done, chunk);
            r = blk_write(fs, ab, bb);
            if (r) {
                sem_release(&fs->sem);
                return done ? (long)done : r;
            }
        }
        done += chunk;
    }

    if (off + len > in->size) {
        in->size = off + len;
        r = inode_store(in);
        if (r) {
            sem_release(&fs->sem);
            return r;
        }
    }

    sem_release(&fs->sem);
    return (long)done;
}

static int ext2_getattr(struct vnode *vn, struct vattr *out)
{
    struct e2_inode *in = vn->priv;

    out->type = in->is_dir ? V_DIR : V_FILE;
    out->size = in->is_dir ? 0 : in->size;
    return 0;
}

/* ---- mount / sniff / mkfs --------------------------------------------------------------------------------------------------------------------------- */

const struct vnode_ops ext2_vops = {
    .lookup   = ext2_lookup,
    .create   = ext2_create,
    .unlink   = ext2_unlink,
    .readdir  = ext2_readdir,
    .read     = ext2_read,
    .write    = ext2_write,
    .getattr  = ext2_getattr,
    .destroy  = ext2_destroy,
};

/* parse + sanity-check a superblock image; fills geometry outputs */
static int sb_parse(const uint8_t *sb /* 1024 */, uint32_t *blocks_out,
                    uint32_t *ngroups_out)
{
    if (le16(&sb[56]) != E2_MAGIC)
        return -EINVAL;
    if (sb[24] != 0)                    /* log_block_size != 1024    */
        return -EINVAL;
    if (le32(&sb[20]) != 1)             /* first_data_block          */
        return -EINVAL;
    if (le16(&sb[88]) != E2_INODE_SIZE)
        return -EINVAL;
    if (le32(&sb[32]) != E2_BPG)        /* blocks_per_group          */
        return -EINVAL;
    if (le32(&sb[76]) < 1)              /* rev level                 */
        return -EINVAL;

    *blocks_out = le32(&sb[4]);
    if (*blocks_out < 64)
        return -EINVAL;
    *ngroups_out =
        (*blocks_out - 1 + E2_BPG - 1) / E2_BPG;
    if (*ngroups_out == 0 || *ngroups_out > E2_NGROUP_MAX)
        return -EINVAL;
    return 0;
}

static int ext2_mount(struct mount *m)
{
    struct e2_fs *fs;
    struct e2_inode *root_in;
    struct vnode *root;
    uint8_t sb[1024];
    uint32_t blocks, ngroups, gdt_blocks;
    int r;

    if (!m->bd)
        return -ENODEV;
    /* the superblock lives at byte 1024 of the volume              */
    r = block_read(m->bd, m->part_lba + E2_BSIZE / BLK_SECTOR_SIZE,
                   sb, E2_BSIZE);
    if (r)
        return r;
    r = sb_parse(sb, &blocks, &ngroups);
    if (r)
        return r;

    gdt_blocks = (ngroups * 32u + E2_BSIZE - 1) / E2_BSIZE;
    if (2 + (uint64_t)gdt_blocks + ngroups * (3 + E2_IPG / 8) >
        blocks)
        return -EINVAL;                 /* descriptors run off disk  */

    fs = kzalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    fs->bd = m->bd;
    fs->part_lba = m->part_lba;
    fs->part_nsect = m->part_nsect;
    fs->blocks_count = blocks;
    fs->ngroups = ngroups;
    kmutex_init(&fs->sem, "ext2");

    /* trust the group descriptors like a real driver would         */
    for (uint32_t g = 0; g < ngroups; g++) {
        uint64_t boff = 2048u + (uint64_t)g * 32u;
        uint8_t sec[512];

        r = block_read(m->bd, m->part_lba + (boff >> 9), sec, 512);
        if (r)
            goto fail;
        fs->grp[g].bbmap  = le32(&sec[boff & 511]);
        fs->grp[g].ibmap  = le32(&sec[(boff & 511) + 4]);
        fs->grp[g].itable = le32(&sec[(boff & 511) + 8]);
        if (!fs->grp[g].itable || !fs->grp[g].bbmap ||
            !fs->grp[g].ibmap) {
            r = -EIO;
            goto fail;
        }
    }

    root_in = kzalloc(sizeof(*root_in));
    if (!root_in) {
        r = -ENOMEM;
        goto fail;
    }
    r = inode_load(fs, E2_ROOT_INO, root_in);
    if (r) {
        kfree(root_in);
        goto fail;
    }
    if (!root_in->is_dir) {
        kfree(root_in);
        r = -EINVAL;
        goto fail;
    }

    root = shell_from(root_in);
    if (!root) {
        kfree(root_in);
        r = -ENOMEM;
        goto fail;
    }
    root->mp = m;
    m->root = root;
    return 0;

fail:
    kfree(fs);
    return r;
}

void ext2_init(void)
{
    vfs_register_fs("ext2", ext2_mount);
}

/* ---- probe + format ------------------------------------------------------------------------------------------------------------------------------ */

int ext2_sniff(struct block_device *bd, uint64_t lba, uint64_t nsect)
{
    uint8_t sb[1024];
    uint32_t blocks, ngroups;

    if (!bd || nsect < 8)
        return 0;
    if (block_read(bd, lba + E2_BSIZE / BLK_SECTOR_SIZE, sb, E2_BSIZE))
        return 0;
    if (sb_parse(sb, &blocks, &ngroups))
        return 0;
    return (uint64_t)blocks * E2_SECT_PER_BLK <= nsect;
}

/*
 * Minimal mke2fs equivalent: rev-1 superblock, 128-byte inodes,
 * reserved inodes 1..10, empty root directory. A small sector tail
 * of the window is deliberately left untouched -- the phase-6 block
 * selftest parks its pattern writes at the very end of the disk, so
 * the filesystem stops short of them.
 */
int ext2_mkfs(struct block_device *bd, uint64_t lba, uint64_t nsect)
{
    struct e2_fs tfs;                   /* scratch geometry view     */
    uint8_t sb[1024];
    uint32_t margin_sect = 32;          /* keep drvtest's tail clear */
    uint32_t usable_sect = (uint32_t)((nsect - margin_sect) &
                                      ~(uint64_t)(E2_SECT_PER_BLK - 1));
    uint32_t blocks = usable_sect / E2_SECT_PER_BLK;
    uint32_t ngroups, gdt_blocks, data0;
    uint32_t free_blocks_total = 0, free_inodes_total = 0;
    static uint8_t zero_blk[E2_BSIZE];
    int r;

    memset(&tfs, 0, sizeof(tfs));

    if (blocks < 4096)
        return -EINVAL;
    ngroups = (blocks - 1 + E2_BPG - 1) / E2_BPG;
    if (ngroups > E2_NGROUP_MAX) {
        ngroups = E2_NGROUP_MAX;
        blocks = (ngroups - 1) * E2_BPG + 1 < blocks
                 ? (ngroups - 1) * E2_BPG + 1 : blocks;
    }
    gdt_blocks = (ngroups * 32u + E2_BSIZE - 1) / E2_BSIZE;

    tfs.bd = bd;
    tfs.part_lba = lba;
    tfs.part_nsect = nsect;
    tfs.blocks_count = blocks;
    tfs.ngroups = ngroups;

    /* per-group layout + freshly zeroed metadata                   */
    for (uint32_t g = 0; g < ngroups; g++) {
        uint32_t gs = g * E2_BPG + 1;
        uint32_t bb = g == 0 ? 2 + gdt_blocks : gs;

        tfs.grp[g].bbmap = bb;
        tfs.grp[g].ibmap = bb + 1;
        tfs.grp[g].itable = bb + 2;

        r = blk_write(&tfs, tfs.grp[g].bbmap, zero_blk);
        if (r)
            return r;
        r = blk_write(&tfs, tfs.grp[g].ibmap, zero_blk);
        if (r)
            return r;
        for (uint32_t t = 0; t < E2_IPG * E2_INODE_SIZE / E2_BSIZE;
             t++) {
            r = blk_write(&tfs, tfs.grp[g].itable + t, zero_blk);
            if (r)
                return r;
        }
    }

    data0 = tfs.grp[0].itable + E2_IPG * E2_INODE_SIZE / E2_BSIZE;

    /* group descriptor table (entries straddle sectors: RMW)       */
    for (uint32_t g = 0; g < ngroups; g++) {
        uint8_t gd[32];
        uint8_t sec[512];
        uint64_t boff = 2048u + (uint64_t)g * 32u;

        memset(gd, 0, sizeof(gd));
        st32(&gd[0], tfs.grp[g].bbmap);
        st32(&gd[4], tfs.grp[g].ibmap);
        st32(&gd[8], tfs.grp[g].itable);

        r = block_read(bd, lba + (boff >> 9), sec, 512);
        if (r)
            return r;
        memcpy(&sec[boff & 511], gd, 32);
        r = block_write(bd, lba + (boff >> 9), sec, 512);
        if (r)
            return r;
    }

    /*
     * Block bitmaps: metadata used, data free, beyond-volume bits
     * used. data0 is per group (each group lays out identically).
     */
    for (uint32_t g = 0; g < ngroups; g++) {
        uint8_t bm[E2_BSIZE];
        uint32_t gs = g * E2_BPG + 1;
        uint32_t dstart = tfs.grp[g].itable +
                          E2_IPG * E2_INODE_SIZE / E2_BSIZE;
        uint32_t gfree = 0;

        memset(bm, 0, sizeof(bm));
        for (uint32_t rel = 0; rel < E2_BPG; rel++) {
            uint32_t ab = gs + rel;

            if (ab >= blocks || ab < dstart ||
                (g == 0 && ab == dstart)) {   /* root dir block     */
                bm[rel >> 3] |= (uint8_t)(1u << (rel & 7));
            } else {
                gfree++;
            }
        }
        r = blk_write(&tfs, tfs.grp[g].bbmap, bm);
        if (r)
            return r;
        free_blocks_total += gfree;
    }

    /* inode bitmaps: inodes 1..10 reserved inside group 0          */
    for (uint32_t g = 0; g < ngroups; g++) {
        uint8_t bm[E2_BSIZE];
        uint32_t ifree_g;

        memset(bm, 0, sizeof(bm));
        if (g == 0) {
            for (uint32_t b = 0; b < 10; b++)
                bm[b >> 3] |= (uint8_t)(1u << (b & 7));
            ifree_g = E2_IPG - 10;
        } else {
            ifree_g = E2_IPG;
        }
        r = blk_write(&tfs, tfs.grp[g].ibmap, bm);
        if (r)
            return r;
        free_inodes_total += ifree_g;
    }

    /* root directory contents (single fresh block)                 */
    {
        uint8_t bb[E2_BSIZE];

        memset(bb, 0, sizeof(bb));
        st32(&bb[0], E2_ROOT_INO);
        st16(&bb[4], 12);
        bb[6] = 1;
        bb[7] = E2_FT_DIR;
        memcpy(&bb[8], ".", 1);
        st32(&bb[12], E2_ROOT_INO);
        st16(&bb[16], E2_BSIZE - 12u);
        bb[22] = 2;
        bb[23] = E2_FT_DIR;
        memcpy(&bb[24], "..", 2);
        r = blk_write(&tfs, data0, bb);
        if (r)
            return r;
    }

    /* root inode (mode drwxr-xr-x, one block)                      */
    {
        uint8_t raw[E2_INODE_SIZE];

        memset(raw, 0, sizeof(raw));
        st16(&raw[0], E2_M_DIR | E2_M_PERM);
        st32(&raw[4], E2_BSIZE);
        st16(&raw[26], 2);              /* ".", ".."                  */
        st32(&raw[28], E2_SECT_PER_BLK);
        st32(&raw[40], data0);
        r = inode_scatter(&tfs, E2_ROOT_INO, raw);
        if (r)
            return r;
    }

    /* superblock last (crash ordering nicety)                      */
    memset(sb, 0, sizeof(sb));
    st32(&sb[0], E2_IPG * ngroups);         /* inodes_count         */
    st32(&sb[4], blocks);                   /* blocks_count         */
    st32(&sb[8], blocks / 10u);             /* reserved blocks      */
    st32(&sb[12], free_blocks_total);
    st32(&sb[16], free_inodes_total);
    st32(&sb[20], 1);                       /* first_data_block     */
    sb[24] = 0;                             /* log_block_size=1024  */
    sb[25] = 0;                             /* log_frag_size        */
    st32(&sb[28], E2_BPG);
    st32(&sb[32], E2_BPG);
    st32(&sb[36], E2_BPG);
    st32(&sb[40], E2_IPG);
    st16(&sb[54], (uint16_t)-1);            /* max_mnt_count        */
    st16(&sb[56], E2_MAGIC);
    st16(&sb[58], 1);                       /* state: clean         */
    st32(&sb[76], 1);                       /* rev level: dynamic   */
    st32(&sb[84], E2_FIRST_INO);
    st16(&sb[88], E2_INODE_SIZE);

    return block_write(bd, lba + E2_BSIZE / BLK_SECTOR_SIZE,
                       sb, E2_BSIZE);
}
