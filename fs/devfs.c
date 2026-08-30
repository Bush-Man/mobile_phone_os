/*
 * devfs.c - device-node filesystem (phase 7).
 *
 * A synthetic filesystem whose directory listing IS the phase-6
 * device registries: every registered char_dev appears as a
 * V_CHARDEV node, every block_device as a V_BLOCKDEV node. Nodes
 * are generated on demand during lookup/readdir -- there is no
 * persistent inode storage to keep in sync with registration order.
 *
 * Character nodes pass read/write straight through the registry ops
 * (the console node therefore speaks canonical tty line discipline).
 * Block nodes expose byte-offset IO over block_read/block_write,
 * bouncing through a sector buffer so any alignment works.
 *
 * The namespace is read-only: create/unlink return -EPERM because
 * devices come into existence through driver (un)registration, not
 * filesystem calls.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "block.h"
#include "chardev.h"
#include "lib.h"
#include "mm/kheap.h"
#include "syscall.h"
#include "vfs.h"

#define DEVFS_BOUNCE BLK_SECTOR_SIZE

/* single ops table for every devfs vnode (see bottom of file); the
 * anonymous stdio console in fs/vfs.c borrows this same table      */
const struct vnode_ops devfs_char_ops;

/* ---- shell helpers ---------------------------------------------------------- */

static struct vnode *shell_make(enum vtype t, void *priv)
{
    struct vnode *vn = kzalloc(sizeof(*vn));

    if (!vn)
        return NULL;
    vn->ops = &devfs_char_ops;
    vn->type = t;
    vn->priv = priv;
    vn->refs = 1;
    return vn;
}

static bool is_root(const struct vnode *vn)
{
    return vn->type == V_DIR;           /* only the root is a dir     */
}

/* ---- directory ------------------------------------------------------------------ */

static int devfs_lookup(struct vnode *dir, const char *name,
                        struct vnode **out)
{
    struct char_dev *cd;
    struct block_device *bd;

    if (!is_root(dir))
        return -ENOTDIR;

    cd = char_dev_find(name);
    if (cd) {
        *out = shell_make(V_CHARDEV, cd);
        return *out ? 0 : -ENOMEM;
    }
    bd = block_find(name);
    if (bd) {
        *out = shell_make(V_BLOCKDEV, bd);
        return *out ? 0 : -ENOMEM;
    }
    return -ENOENT;
}

/*
 * Index space: [0, nchar) lists character devices in registry
 * order, then [nchar, nchar + nblk) lists block devices.
 */
static int devfs_readdir(struct vnode *dir, unsigned idx,
                         char *name_out, uint8_t *type_out)
{
    unsigned nchar = char_dev_count();
    struct char_dev *cd;
    struct block_device *bd;

    if (!is_root(dir))
        return -ENOTDIR;

    cd = char_dev_at(idx);
    if (cd) {
        kstrlcpy(name_out, cd->name, VFS_NAME_MAX);
        *type_out = DT_CHR;
        return 0;
    }
    bd = block_at(idx - nchar);
    if (idx >= nchar && bd) {
        kstrlcpy(name_out, bd->name, VFS_NAME_MAX);
        *type_out = DT_BLK;
        return 0;
    }
    return -ENOENT;
}

/* ---- chardev passthrough -------------------------------------------------------------- */

static long devfs_char_read(struct vnode *vn, uint64_t off,
                            void *buf, size_t len)
{
    struct char_dev *cd = vn->priv;

    (void)off;                          /* streams have no offsets    */
    if (!cd->read)
        return -EBADF;                  /* write-only node            */
    if (len > 0x10000u)
        len = 0x10000u;
    return cd->read(cd, buf, (unsigned)len);
}

static long devfs_char_write(struct vnode *vn, uint64_t off,
                             const void *buf, size_t len)
{
    struct char_dev *cd = vn->priv;

    (void)off;
    if (!cd->write)
        return -EBADF;                  /* read-only stream           */
    if (len > 0x10000u)
        len = 0x10000u;
    return cd->write(cd, buf, (unsigned)len);
}

/* ---- byte-offset block IO ----------------------------------------------------------------- */

static long devfs_block_rw(struct vnode *vn, uint64_t off, void *buf,
                           size_t len, bool write)
{
    struct block_device *bd = vn->priv;
    uint64_t cap_bytes = bd->capacity_sectors * BLK_SECTOR_SIZE;
    uint8_t bounce[DEVFS_BOUNCE];
    size_t done = 0;

    if (off >= cap_bytes)
        return 0;
    if ((uint64_t)len > cap_bytes - off)
        len = (size_t)(cap_bytes - off);

    while (done < len) {
        uint64_t lba = (off + done) >> BLK_SECTOR_SHIFT;
        size_t doff = (off + done) & (BLK_SECTOR_SIZE - 1);
        size_t chunk = BLK_SECTOR_SIZE - doff;
        int r;

        if (chunk > len - done)
            chunk = len - done;

        if (write && chunk == BLK_SECTOR_SIZE) {
            r = block_write(bd, lba, (uint8_t *)buf + done,
                            BLK_SECTOR_SIZE);
        } else {
            r = block_read(bd, lba, bounce, BLK_SECTOR_SIZE);
            if (!r && chunk) {
                if (write)
                    memcpy(&bounce[doff], (uint8_t *)buf + done,
                           chunk);
                else
                    memcpy((uint8_t *)buf + done, &bounce[doff],
                           chunk);
                if (write)
                    r = block_write(bd, lba, bounce,
                                    BLK_SECTOR_SIZE);
            }
        }
        if (r)
            return done ? (long)done : -EIO;
        done += chunk;
    }
    return (long)done;
}

/* ---- dispatch ------------------------------------------------------------------------------- */

static long devfs_read(struct vnode *vn, uint64_t off,
                      void *buf, size_t len)
{
    switch (vn->type) {
    case V_CHARDEV:
        return devfs_char_read(vn, off, buf, len);
    case V_BLOCKDEV:
        return devfs_block_rw(vn, off, buf, len, false);
    default:
        return -EISDIR;
    }
}

static long devfs_write(struct vnode *vn, uint64_t off,
                       const void *buf, size_t len)
{
    switch (vn->type) {
    case V_CHARDEV:
        return devfs_char_write(vn, off, buf, len);
    case V_BLOCKDEV:
        return devfs_block_rw(vn, off, (void *)buf, len, true);
    default:
        return -EISDIR;
    }
}

static int devfs_getattr(struct vnode *vn, struct vattr *out)
{
    if (vn->type == V_BLOCKDEV) {
        struct block_device *bd = vn->priv;

        out->size = bd->capacity_sectors * BLK_SECTOR_SIZE;
    } else {
        out->size = 0;
    }
    out->type = vn->type;
    return 0;
}

static void devfs_destroy(struct vnode *vn)
{
    kfree(vn);                          /* shells are heap-only       */
}

/*
 * The one ops table for all devfs vnodes (and the borrowed console
 * shell). create/unlink are NULL: the namespace is registry-owned.
 */
const struct vnode_ops devfs_char_ops = {
    .lookup   = devfs_lookup,
    .create   = NULL,
    .unlink   = NULL,
    .readdir  = devfs_readdir,
    .read     = devfs_read,
    .write    = devfs_write,
    .getattr  = devfs_getattr,
    .destroy  = devfs_destroy,
};

/* ---- mount -------------------------------------------------------------------------------------- */

/*
 * The root directory is a static immortal vnode (the mount holds a
 * reference for its whole life; memory for one shell is not worth a
 * free path that can never run).
 */
static struct vnode devfs_root;

static int devfs_mount(struct mount *m)
{
    if (!devfs_root.ops) {
        memset(&devfs_root, 0, sizeof(devfs_root));
        devfs_root.ops = &devfs_char_ops;
        devfs_root.type = V_DIR;
        devfs_root.refs = 1;
    }
    devfs_root.mp = m;
    m->root = &devfs_root;
    return 0;
}

void devfs_init(void)
{
    vfs_register_fs("devfs", devfs_mount);
}
