/*
 * ramfs.c - in-memory filesystem (phase 7).
 *
 * Serves as the early-boot root ("/"): directories are linked lists
 * of nodes, file data is a single growable kmalloc buffer. Nothing
 * survives reboot -- by design; disk-backed filesystems mount under
 * it once the block stack is up.
 *
 * Lifetime model: a ram_node is refcounted THROUGH its vnode shells
 * (node->vn_live). unlink() detaches a node from its parent and
 * marks it orphaned; the storage is freed either immediately (no
 * live shells) or by ops->destroy() when the last shell dies. This
 * keeps an open fd usable after its entry is unlinked, like POSIX.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lib.h"
#include "mm/kheap.h"
#include "syscall.h"
#include "vfs.h"

#define RAMFS_MAX_FILE (256u * 1024u)   /* heap-protection cap       */
#define GROW_MIN        128
#define GROW_MAX     (64u * 1024u)      /* doubling ceiling          */

const struct vnode_ops ramfs_ops;

struct ram_node {
    char name[VFS_NAME_MAX];
    enum vtype type;

    struct ram_node *sibling;           /* next child, same parent   */
    struct ram_node *children;          /* first child (dirs)        */
    struct ram_node *parent;

    uint8_t *data;                      /* files                     */
    size_t len;
    size_t cap;

    struct ram_fs *fs;
    uint64_t ino;
    bool orphan;                        /* detached; die with refs   */
    unsigned vn_live;                   /* live vnode shells         */
};

struct ram_fs {
    struct ram_node root;
    spinlock_t lock;
    uint64_t next_ino;
};

/* ---- vnode shells ---------------------------------------------------------- */

static struct vnode *shell_make(struct ram_node *n)
{
    struct vnode *vn = kzalloc(sizeof(*vn));

    if (!vn)
        return NULL;
    vn->ops = &ramfs_ops;
    vn->type = n->type;
    vn->priv = n;
    vn->ino = n->ino;
    n->vn_live++;
    return vn;
}

static void ramfs_destroy(struct vnode *vn)
{
    struct ram_node *n = vn->priv;
    struct ram_fs *fs = n->fs;
    daif_state s;

    kfree(vn);

    spin_lock_irqsave(&fs->lock, &s);
    if (--n->vn_live == 0 && n->orphan) {
        spin_unlock_irqrestore(&fs->lock, s);
        kfree(n->data);
        kfree(n);
        return;
    }
    spin_unlock_irqrestore(&fs->lock, s);
}

/* ---- directory operations ------------------------------------------------------ */

static struct ram_node *find_child(struct ram_node *dir,
                                   const char *name)
{
    for (struct ram_node *c = dir->children; c; c = c->sibling)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

static int ramfs_lookup(struct vnode *dir, const char *name,
                        struct vnode **out)
{
    struct ram_node *d = dir->priv, *c;
    struct ram_fs *fs = d->fs;
    daif_state s;

    if (!name[0])
        return -ENOENT;

    spin_lock_irqsave(&fs->lock, &s);
    c = find_child(d, name);
    if (c)
        *out = shell_make(c);           /* refs = 1                   */
    spin_unlock_irqrestore(&fs->lock, s);

    return c ? 0 : -ENOENT;
}

static int ramfs_create(struct vnode *dir, const char *name,
                        enum vtype t, struct vnode **out)
{
    struct ram_node *d = dir->priv;
    struct ram_fs *fs = d->fs;
    struct ram_node *n;
    daif_state s;

    if (strlen(name) >= VFS_NAME_MAX)
        return -ENAMETOOLONG;

    n = kzalloc(sizeof(*n));
    if (!n)
        return -ENOMEM;

    spin_lock_irqsave(&fs->lock, &s);
    if (find_child(d, name)) {
        spin_unlock_irqrestore(&fs->lock, s);
        kfree(n);
        return -EEXIST;
    }

    kstrlcpy(n->name, name, VFS_NAME_MAX);
    n->type = t;
    n->parent = d;
    n->fs = fs;
    n->ino = ++fs->next_ino;

    n->sibling = d->children;           /* push front                 */
    d->children = n;

    *out = shell_make(n);
    spin_unlock_irqrestore(&fs->lock, s);

    if (!*out)
        return -ENOMEM;
    return 0;
}

static void drop_node(struct ram_node *n)
{
    if (n->orphan && !n->vn_live) {
        kfree(n->data);
        kfree(n);
    }
}

static int ramfs_unlink(struct vnode *dir, const char *name)
{
    struct ram_node *d = dir->priv;
    struct ram_fs *fs = d->fs;
    struct ram_node **pp, *c;
    daif_state s;

    spin_lock_irqsave(&fs->lock, &s);
    for (pp = &d->children; (c = *pp); pp = &c->sibling) {
        if (strcmp(c->name, name) != 0)
            continue;
        if (c->type == V_DIR && c->children) {
            spin_unlock_irqrestore(&fs->lock, s);
            return -ENOTEMPTY;
        }
        *pp = c->sibling;               /* detach                     */
        c->sibling = NULL;
        c->parent = NULL;
        c->orphan = true;
        drop_node(c);                   /* frees iff no live shells   */
        spin_unlock_irqrestore(&fs->lock, s);
        return 0;
    }
    spin_unlock_irqrestore(&fs->lock, s);
    return -ENOENT;
}

static int ramfs_readdir(struct vnode *dir, unsigned idx,
                         char *name_out, uint8_t *type_out)
{
    struct ram_node *d = dir->priv;
    struct ram_fs *fs = d->fs;
    struct ram_node *c;
    daif_state s;

    spin_lock_irqsave(&fs->lock, &s);
    c = d->children;
    while (c && idx--)
        c = c->sibling;
    if (c) {
        memcpy(name_out, c->name, strlen(c->name) + 1);
        *type_out = c->type == V_DIR ? DT_DIR : DT_REG;
    }
    spin_unlock_irqrestore(&fs->lock, s);

    return c ? 0 : -ENOENT;
}

/* ---- file data -------------------------------------------------------------------- */

static long ramfs_read(struct vnode *vn, uint64_t off,
                      void *buf, size_t len)
{
    struct ram_node *n = vn->priv;
    struct ram_fs *fs = n->fs;
    daif_state s;

    if (vn->type != V_FILE)
        return -EISDIR;

    spin_lock_irqsave(&fs->lock, &s);
    if (off >= n->len) {
        spin_unlock_irqrestore(&fs->lock, s);
        return 0;                       /* EOF                        */
    }
    if (off + len > n->len)
        len = n->len - off;
    memcpy(buf, n->data + off, len);
    spin_unlock_irqrestore(&fs->lock, s);

    return (long)len;
}

static long ramfs_write(struct vnode *vn, uint64_t off,
                       const void *buf, size_t len)
{
    struct ram_node *n = vn->priv;
    struct ram_fs *fs = n->fs;
    daif_state s;

    if (vn->type != V_FILE)
        return -EISDIR;
    /* zero-length write is the truncate idiom from vfs_open()      */
    if (!len && off == 0) {
        spin_lock_irqsave(&fs->lock, &s);
        n->len = 0;
        spin_unlock_irqrestore(&fs->lock, s);
        return 0;
    }

    spin_lock_irqsave(&fs->lock, &s);
    if (off + len > RAMFS_MAX_FILE) {
        spin_unlock_irqrestore(&fs->lock, s);
        return -ENOSPC;
    }
    if (off + len > n->cap) {
        size_t want = off + len;
        size_t cap = n->cap ? n->cap : GROW_MIN;
        uint8_t *fresh;

        while (cap < want && cap < GROW_MAX)
            cap *= 2;
        if (cap < want)
            cap = want;
        fresh = kmalloc(cap);
        if (!fresh) {
            spin_unlock_irqrestore(&fs->lock, s);
            return -ENOMEM;
        }
        memcpy(fresh, n->data, n->len);
        memset(fresh + n->len, 0, cap - n->len);
        kfree(n->data);
        n->data = fresh;
        n->cap = cap;
    }
    if (off > n->len)
        memset(n->data + n->len, 0, (size_t)(off - n->len));
    memcpy(n->data + off, buf, len);
    if (off + len > n->len)
        n->len = off + len;
    spin_unlock_irqrestore(&fs->lock, s);

    return (long)len;
}

static int ramfs_getattr(struct vnode *vn, struct vattr *out)
{
    struct ram_node *n = vn->priv;

    out->size = n->type == V_FILE ? n->len : 0;
    return 0;
}

/* ---- registration ------------------------------------------------------------------------ */

const struct vnode_ops ramfs_ops = {
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .unlink   = ramfs_unlink,
    .readdir  = ramfs_readdir,
    .read     = ramfs_read,
    .write    = ramfs_write,
    .getattr  = ramfs_getattr,
    .destroy  = ramfs_destroy,
};

/*
 * Mount function: build one fresh empty tree as the mount root.
 * Called with the claimed struct mount; fills m->root (ref held by
 * the mount forever -- memory filesystems never unmount here).
 */
static int ramfs_mount(struct mount *m)
{
    struct ram_fs *fs = kzalloc(sizeof(*fs));

    if (!fs)
        return -ENOMEM;

    fs->lock = (spinlock_t)SPINLOCK_INIT;
    fs->root.type = V_DIR;
    fs->root.fs = fs;
    fs->next_ino = 1;

    {
        struct vnode *r = shell_make(&fs->root);

        if (!r) {
            kfree(fs);
            return -ENOMEM;
        }
        r->mp = m;
        m->root = r;
    }

    return 0;
}

void ramfs_init(void)
{
    vfs_register_fs("ramfs", ramfs_mount);
}
