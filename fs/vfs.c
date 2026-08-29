/*
 * vfs.c - virtual filesystem core (phase 7).
 *
 * Owns four things:
 *
 *   1. the filesystem-type registry ("ramfs", "devfs", "vfat",
 *      "ext2" -> mount functions),
 *   2. the mount table: one flat namespace, mounts keyed by their
 *      absolute mountpoint path,
 *   3. path resolution: lexical normalization of "."/".." followed
 *      by a linear walk from the root, crossing mountpoints by
 *      string comparison as directories are entered,
 *   4. struct file + per-process fd tables and the process
 *      lifecycle hooks proc.c calls at spawn/fork/reap.
 *
 * Locking: one global irq-safe lock guards the registries, mount
 * table and all refcounts. Filesystem internals take their own
 * locks on top; no fs operation is ever invoked while vfs_lock is
 * held.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "block.h"
#include "chardev.h"
#include "lib.h"
#include "mm/kheap.h"
#include "panic.h"
#include "proc.h"
#include "syscall.h"
#include "task.h"
#include "tty.h"
#include "usabi.h"
#include "vfs.h"

struct fs_type {
    const char *name;
    fs_mount_fn mount_fn;
};

#define VFS_COMPS_MAX 24        /* components resolved per walk     */

static spinlock_t vfs_lock = SPINLOCK_INIT;

static struct fs_type fstypes[8];
static unsigned nfstypes;

static struct mount mounts[VFS_MOUNT_MAX];
static unsigned nmounts;
static bool have_root;

/* ---- helpers ------------------------------------------------------------ */

static void vlock(daif_state *s)
{
    spin_lock_irqsave(&vfs_lock, s);
}

static void vunlock(daif_state s)
{
    spin_unlock_irqrestore(&vfs_lock, s);
}

/*
 * Join directory prefix + component: base "" means the namespace
 * root, so the result is "/comp"; otherwise base already looks like
 * "/a/b" and the result is "/a/b/comp". dst may alias base.
 */
static void path_join(char *dst, size_t cap,
                      const char *base, const char *comp)
{
    size_t bl = strlen(base);
    size_t cl = strlen(comp);
    char tmp[VFS_PATH_MAX];
    size_t i = 0;

    if (bl + 1 + cl + 1 > cap)
        cl = 0;                         /* clamp: caller re-checks    */

    if (bl)
        memcpy(tmp, base, bl), i = bl;
    tmp[i++] = '/';
    memcpy(&tmp[i], comp, cl);
    tmp[i + cl] = '\0';

    memcpy(dst, tmp, i + cl + 1);
}

/* last '/' of s, like strrchr(s, '/') */
static const char *last_slash(const char *s)
{
    const char *found = NULL;

    for (; *s; s++)
        if (*s == '/')
            found = s;
    return found;
}

/* ---- refs ---------------------------------------------------------------- */

void vn_ref(struct vnode *vn)
{
    daif_state s;

    if (!vn)
        return;
    vlock(&s);
    vn->refs++;
    vunlock(s);
}

void vn_unref(struct vnode *vn)
{
    daif_state s;
    unsigned left;

    if (!vn)
        return;

    vlock(&s);
    left = --vn->refs;
    vunlock(s);

    if (!left && vn->ops && vn->ops->destroy)
        vn->ops->destroy(vn);           /* frees the shell too        */
}

int vfs_getattr(struct vnode *vn, struct vattr *out)
{
    if (!vn || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->type = vn->type;
    if (!vn->ops || !vn->ops->getattr)
        return 0;                       /* type-only answer is fine   */
    return vn->ops->getattr(vn, out);
}

/* ---- op wrappers ----------------------------------------------------------- */

int vfs_lookup_at(struct vnode *dir, const char *name, struct vnode **out)
{
    int r;

    if (!dir || !name || !out)
        return -EINVAL;
    if (dir->type != V_DIR)
        return -ENOTDIR;
    if (!dir->ops->lookup)
        return -EPERM;
    r = dir->ops->lookup(dir, name, out);
    if (!r && !*out)
        return -ENOENT;
    return r;
}

int vfs_create_at(struct vnode *dir, const char *name, enum vtype t,
                  struct vnode **out)
{
    if (!dir || !name || !out)
        return -EINVAL;
    if (dir->type != V_DIR)
        return -ENOTDIR;
    if (t != V_FILE && t != V_DIR)
        return -EINVAL;
    if (!dir->ops->create)
        return -EPERM;
    return dir->ops->create(dir, name, t, out);
}

int vfs_unlink_at(struct vnode *dir, const char *name)
{
    if (!dir || !name)
        return -EINVAL;
    if (dir->type != V_DIR)
        return -ENOTDIR;
    if (!dir->ops->unlink)
        return -EPERM;
    return dir->ops->unlink(dir, name);
}

int vfs_readdir_at(struct vnode *dir, unsigned idx,
                   char *name_out, uint8_t *type_out)
{
    if (!dir || !name_out || !type_out)
        return -EINVAL;
    if (dir->type != V_DIR)
        return -ENOTDIR;
    if (!dir->ops->readdir)
        return -EPERM;
    return dir->ops->readdir(dir, idx, name_out, type_out);
}

/* ---- subsystem + mounts ------------------------------------------------------- */

void vfs_subsys_init(void)
{
    kprintf("vfs: core ready\n");
}

int vfs_register_fs(const char *fstype, fs_mount_fn mount_fn)
{
    daif_state s;

    if (!fstype || !mount_fn)
        return -EINVAL;
    vlock(&s);
    for (unsigned i = 0; i < nfstypes; i++)
        if (strcmp(fstypes[i].name, fstype) == 0) {
            vunlock(s);
            return -EEXIST;
        }
    if (nfstypes >= ARRAY_SIZE(fstypes)) {
        vunlock(s);
        return -ENOMEM;
    }
    fstypes[nfstypes].name = fstype;
    fstypes[nfstypes].mount_fn = mount_fn;
    nfstypes++;
    vunlock(s);
    return 0;
}

unsigned vfs_mount_count(void)
{
    return nmounts;
}

unsigned vfs_mount_count(void)
{
    return nmounts;
}

/*
 * Phase 14: fill up to `max` mountinfo records (usabi.h layout) for
 * the SYS_mountinfo report. Returns the number of entries written.
 * Snapshot runs under the registry lock, so an active mount never
 * appears half-copied.
 */
unsigned vfs_mountinfo_fill(struct mountinfo_entry *ents, unsigned max)
{
    daif_state s;
    unsigned out = 0;

    if (!ents)
        return 0;

    vlock(&s);
    for (unsigned i = 0; i < nmounts && out < max; i++) {
        if (!mounts[i].active)
            continue;

        memset(ents[out].fstype, 0, sizeof(ents[out].fstype));
        kstrlcpy(ents[out].fstype,
                 mounts[i].fstype ? mounts[i].fstype : "?",
                 sizeof(ents[out].fstype));
        memset(ents[out].path, 0, sizeof(ents[out].path));
        kstrlcpy(ents[out].path, mounts[i].path, sizeof(ents[out].path));
        out++;
    }
    vunlock(s);
    return out;
}

bool vfs_path_is_mounted(const char *path)
bool vfs_path_is_mounted(const char *path)
{
    daif_state s;
    bool found = false;

    vlock(&s);
    for (unsigned i = 0; i < nmounts; i++)
        if (mounts[i].active && strcmp(mounts[i].path, path) == 0) {
            found = true;
            break;
        }
    vunlock(s);
    return found;
}

struct vnode *vfs_root(void)
{
    struct vnode *r = NULL;
    daif_state s;

    vlock(&s);
    if (have_root) {
        r = mounts[0].root;             /* slot 0 is always "/"       */
        r->refs++;
    }
    vunlock(s);
    return r;
}

/*
 * Mount `fstype` at absolute `path`. The fs-specific mount function
 * receives the claimed slot and must fill ->root (the mount keeps a
 * reference). Mounting over an existing mountpoint, or anything but
 * "/" before a root exists, is rejected.
 */
int vfs_mount(const char *fstype, const char *path,
              struct block_device *bd, uint64_t lba, uint64_t nsect)
{
    struct fs_type *ft = NULL;
    struct mount *m;
    daif_state s;
    int r;

    if (!fstype || !path || path[0] != '/' ||
        strlen(path) >= VFS_PATH_MAX)
        return -EINVAL;

    vlock(&s);
    for (unsigned i = 0; i < nfstypes; i++)
        if (strcmp(fstypes[i].name, fstype) == 0) {
            ft = &fstypes[i];
            break;
        }
    if (!ft) {
        vunlock(s);
        return -ENODEV;
    }

    bool is_root = strcmp(path, "/") == 0;

    if ((is_root && have_root) || (!is_root && !have_root)) {
        vunlock(s);
        return is_root ? -EBUSY : -ENOENT;
    }
    for (unsigned i = 0; i < nmounts; i++)
        if (mounts[i].active && strcmp(mounts[i].path, path) == 0) {
            vunlock(s);
            return -EBUSY;
        }
    if (nmounts >= VFS_MOUNT_MAX) {
        vunlock(s);
        return -ENOMEM;
    }

    m = &mounts[nmounts];
    memset(m, 0, sizeof(*m));
    m->fstype = ft->name;
    m->bd = bd;
    m->part_lba = lba;
    m->part_nsect = nsect;
    kstrlcpy(m->path, path, VFS_PATH_MAX);

    m->active = true;                   /* claim the slot             */
    nmounts++;
    if (is_root)
        have_root = true;
    vunlock(s);

    r = ft->mount_fn(m);
    if (r) {
        vlock(&s);
        m->active = false;
        nmounts--;
        if (is_root)
            have_root = false;
        vunlock(s);
        return r;
    }
    if (!m->root)
        panic("vfs: mount fn left no root vnode");

    kprintf("vfs: mounted %s%s%s%s on %s\n", ft->name,
            bd ? " [" : "", bd ? bd->name : "", bd ? "]" : "", path);
    return 0;
}

/* ---- path resolution --------------------------------------------------------------- */

/*
 * Lexical normalization: split into components, resolving "." and
 * ".." with clamping at "/" (no symlinks exist, so lexical == real).
 * Returns the component count or -ENAMETOOLONG.
 */
static int normalize_path(const char *path, char comps[][VFS_NAME_MAX])
{
    unsigned n = 0;
    const char *s = path;

    while (*s) {
        unsigned len = 0;

        while (*s == '/')
            s++;
        if (!*s)
            break;
        while (s[len] && s[len] != '/')
            len++;

        if (len == 1 && s[0] == '.') {
            s += len;
            continue;
        }
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            s += len;
            if (n)
                n--;                    /* clamp above root           */
            continue;
        }
        if (n >= VFS_COMPS_MAX)
            return -ENAMETOOLONG;
        {
            size_t copy = len < VFS_NAME_MAX - 1
                          ? len : VFS_NAME_MAX - 1;

            memcpy(comps[n], s, copy);
            comps[n][copy] = '\0';
            n++;
        }
        s += len;
    }
    return (int)n;
}

static struct mount *find_mountpoint_locked(const char *path)
{
    for (unsigned i = 0; i < nmounts; i++)
        if (mounts[i].active && strcmp(mounts[i].path, path) == 0)
            return &mounts[i];
    return NULL;
}

int vfs_resolve(const char *path, struct vnode **out)
{
    char comps[VFS_COMPS_MAX][VFS_NAME_MAX];
    char cur_path[VFS_PATH_MAX];        /* "" at root; never "/"      */
    struct vnode *cur;
    int ncomps;
    daif_state s;

    if (!path || !out || path[0] != '/')
        return -EINVAL;

    ncomps = normalize_path(path, comps);
    if (ncomps < 0)
        return ncomps;

    vlock(&s);
    cur = NULL;
    if (have_root) {
        cur = mounts[0].root;
        cur->refs++;
    }
    vunlock(s);
    if (!cur)
        return -ENOENT;

    cur_path[0] = '\0';

    for (int i = 0; i < ncomps; i++) {
        struct vnode *next = NULL;
        struct mount *x;
        char trial[VFS_PATH_MAX];

        if (cur->type != V_DIR) {
            vn_unref(cur);
            return -ENOTDIR;
        }

        /*
         * Cross into a mounted filesystem exactly when the walk
         * arrives at its mountpoint directory.
         */
        path_join(trial, VFS_PATH_MAX, cur_path, comps[i]);

        vlock(&s);
        x = find_mountpoint_locked(trial);
        if (x)
            x->root->refs++;
        vunlock(s);

        if (x) {
            next = x->root;
            memcpy(cur_path, trial, strlen(trial) + 1);
        } else {
            int r = vfs_lookup_at(cur, comps[i], &next);

            if (r) {
                vn_unref(cur);
                return r;
            }
            path_join(cur_path, VFS_PATH_MAX, cur_path, comps[i]);
        }

        vn_unref(cur);
        cur = next;
    }

    *out = cur;
    return 0;
}

/* ---- whole-path operations ------------------------------------------------------------- */

/* split "/a/b/c" -> parent "/a/b" (or "/"), base "c"; 0 on success */
static int split_parent(const char *path, char *parent, char *base)
{
    const char *last = last_slash(path);
    size_t plen;

    if (!last)
        return -EINVAL;
    plen = (size_t)(last - path);
    if (!plen)
        plen = 1;                       /* keep the leading slash     */
    if (plen >= VFS_PATH_MAX || strlen(last + 1) >= VFS_NAME_MAX)
        return -ENAMETOOLONG;

    memcpy(parent, path, plen);
    parent[plen] = '\0';
    kstrlcpy(base, last + 1, VFS_NAME_MAX);
    return base[0] ? 0 : -EINVAL;
}

/* "." / ".." / "" are never valid final names for create/unlink   */
static bool bad_leaf_name(const char *base)
{
    return !base[0] || strcmp(base, ".") == 0 || strcmp(base, "..") == 0;
}

int vfs_open(const char *path, unsigned flags, struct file **out)
{
    struct vnode *vn = NULL;
    struct vattr va;
    unsigned amode = flags & O_ACCMODE;
    int r;

    if (!path || !out || amode == 3u)
        return -EINVAL;

    r = vfs_resolve(path, &vn);
    if (r == -ENOENT && (flags & O_CREAT)) {
        char parent[VFS_PATH_MAX], base[VFS_NAME_MAX];
        struct vnode *pdir;

        r = split_parent(path, parent, base);
        if (r)
            return r;
        if (bad_leaf_name(base))
            return -EEXIST;

        r = vfs_resolve(parent, &pdir);
        if (r)
            return r;
        r = vfs_create_at(pdir, base, V_FILE, &vn);
        vn_unref(pdir);
        if (r)
            return r;
    }
    if (r)
        return r;

    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vn_unref(vn);
        return -EEXIST;
    }
    if (vfs_getattr(vn, &va)) {
        vn_unref(vn);
        return -EIO;
    }
    if (va.type == V_DIR && amode != O_RDONLY) {
        vn_unref(vn);
        return -EISDIR;
    }
    if ((flags & O_TRUNC) && vn->ops && vn->ops->write &&
        va.type != V_DIR)
        vn->ops->write(vn, 0, NULL, 0);     /* zero-length truncate */

    *out = file_alloc(vn, flags);       /* consumes our ref           */
    if (!*out) {
        vn_unref(vn);
        return -ENOMEM;
    }
    return 0;
}

int vfs_unlink(const char *path)
{
    char parent[VFS_PATH_MAX], base[VFS_NAME_MAX];
    struct vnode *pdir;
    int r;

    if (!path || path[0] != '/')
        return -EINVAL;
    r = split_parent(path, parent, base);
    if (r)
        return r;
    if (bad_leaf_name(base))
        return -EINVAL;

    r = vfs_resolve(parent, &pdir);
    if (r)
        return r;
    r = vfs_unlink_at(pdir, base);
    vn_unref(pdir);
    return r;
}

static int path_create_dir(const char *path, enum vtype t)
{
    char parent[VFS_PATH_MAX], base[VFS_NAME_MAX];
    struct vnode *pdir, *fresh = NULL;
    int r;

    if (!path || path[0] != '/')
        return -EINVAL;
    r = split_parent(path, parent, base);
    if (r)
        return r;
    if (bad_leaf_name(base))
        return -EEXIST;

    r = vfs_resolve(parent, &pdir);
    if (r)
        return r;
    r = vfs_create_at(pdir, base, t, &fresh);
    if (fresh)
        vn_unref(fresh);
    vn_unref(pdir);
    return r;
}

int vfs_mkdir(const char *path)
{
    return path_create_dir(path, V_DIR);
}

int vfs_rmdir(const char *path)
{
    struct vnode *vn = NULL;
    int r;

    r = vfs_resolve(path, &vn);
    if (r)
        return r;
    if (vn->type != V_DIR) {
        vn_unref(vn);
        return -ENOTDIR;
    }
    vn_unref(vn);
    return vfs_unlink(path);            /* fs verifies emptiness      */
}

/* ---- files ------------------------------------------------------------------------------ */

struct file *file_alloc(struct vnode *vn, unsigned flags)
{
    struct file *f = kzalloc(sizeof(*f));

    if (!f)
        return NULL;
    f->vn = vn;                         /* consumes one reference     */
    f->flags = flags;
    f->refs = 1;
    return f;
}

void file_close(struct file *f)
{
    daif_state s;
    unsigned left;

    if (!f)
        return;

    vlock(&s);
    left = --f->refs;
    vunlock(s);

    if (!left) {
        vn_unref(f->vn);
        kfree(f);
    }
}

long f_read(struct file *f, void *buf, size_t len)
{
    if (!f || !buf)
        return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (!f->vn->ops || !f->vn->ops->read)
        return -EINVAL;
    return f->vn->ops->read(f->vn, f->off, buf, len);
}

long f_write(struct file *f, const void *buf, size_t len)
{
    struct vattr va;
    long r;

    if (!f)
        return -EINVAL;
    if (!buf && len)
        return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (!f->vn->ops || !f->vn->ops->write)
        return -EINVAL;

    if (f->flags & O_APPEND) {
        if (vfs_getattr(f->vn, &va))
            return -EIO;
        f->off = va.size;
    }

    r = f->vn->ops->write(f->vn, f->off, buf, len);
    if (r > 0)
        f->off += (uint64_t)r;
    return r;
}

int f_lseek(struct file *f, int64_t off, int whence, int64_t *out)
{
    struct vattr va;
    int64_t pos;
    int r;

    if (!f)
        return -EINVAL;
    if (f->vn->type == V_DIR)
        return -EISDIR;
    if (f->vn->type == V_PIPE || f->vn->type == V_SOCK)
        return -ESPIPE;             /* IPC endpoints are not seekable */

    switch (whence) {
    case SEEK_SET:
        pos = off;
        break;
    case SEEK_CUR:
        pos = (int64_t)f->off + off;
        break;
    case SEEK_END:
        r = vfs_getattr(f->vn, &va);
        if (r)
            return r;
        pos = (int64_t)va.size + off;
        break;
    default:
        return -EINVAL;
    }
    if (pos < 0)
        return -EINVAL;
    f->off = (uint64_t)pos;
    if (out)
        *out = pos;
    return 0;
}

/*
 * Fill kbuf with packed ux_dirent records starting at the dir
 * cookie f->off. Returns bytes filled; 0 means end-of-directory
 * (or that not even one more record fits).
 */
long f_getdents(struct file *f, void *kbuf, size_t buflen)
{
    uint8_t *p = kbuf;
    size_t used = 0;

    if (!f || !kbuf)
        return -EINVAL;
    if (f->vn->type != V_DIR)
        return -ENOTDIR;

    while (used < buflen) {
        char name[VFS_NAME_MAX];
        uint8_t dtype;
        size_t reclen;
        int r;

        r = vfs_readdir_at(f->vn, (unsigned)f->off, name, &dtype);
        if (r == -ENOENT)
            break;
        if (r)
            return r;

        reclen = ux_dirent_len(name);
        if (used + reclen > buflen)
            break;                      /* cookie NOT consumed        */

        f->off++;
        *(uint16_t *)(void *)p = (uint16_t)reclen;
        p[2] = dtype;
        memcpy(&p[3], name, strlen(name) + 1);
        used += reclen;
        p += reclen;
    }
    return (long)used;
}

/* ---- fd tables --------------------------------------------------------------------------- */

int vfs_fd_install(struct fd_table *t, struct file *f)
{
    daif_state s;

    if (!t || !f)
        return -EINVAL;
    spin_lock_irqsave(&t->lock, &s);
    for (int fd = 0; fd < PROC_FD_MAX; fd++) {
        if (!t->files[fd]) {
            t->files[fd] = f;
            spin_unlock_irqrestore(&t->lock, s);
            return fd;
        }
    }
    spin_unlock_irqrestore(&t->lock, s);
    return -EMFILE;
}

struct file *vfs_fd_get(struct fd_table *t, int fd)
{
    struct file *f = NULL;
    daif_state s;

    if (!t || fd < 0 || fd >= PROC_FD_MAX)
        return NULL;
    spin_lock_irqsave(&t->lock, &s);
    f = t->files[fd];
    if (f)
        f->refs++;                      /* caller owes a close        */
    spin_unlock_irqrestore(&t->lock, s);
    return f;
}

struct proc *vfs_current_proc(void)
{
    struct task *t = current_task();

    return t ? t->proc : NULL;
}

struct vnode *vfs_vnode_of_fd(int fd)
{
    struct proc *p = vfs_current_proc();
    struct file *f;
    struct vnode *vn;

    if (!p || !p->fds || fd < 0 || fd >= PROC_FD_MAX)
        return NULL;
    f = vfs_fd_get(p->fds, fd);
    if (!f)
        return NULL;
    vn = f->vn;
    file_close(f);
    return vn;
}

int vfs_install_vnode(struct vnode *vn)
{
    struct proc *p = vfs_current_proc();
    struct file *f;
    int fd;

    if (!p || !vn)
        return -1;
    if (!p->fds && vfs_proc_fds_init(p))
        return -1;
    f = file_alloc(vn, O_RDWR);
    if (!f)
        return -1;
    fd = vfs_fd_install(p->fds, f);
    if (fd < 0)
        file_close(f);
    return fd;
}

void vfs_fd_put(struct fd_table *t, int fd)
{
    struct file *f = NULL;
    daif_state s;

    if (!t || fd < 0 || fd >= PROC_FD_MAX)
        return;
    spin_lock_irqsave(&t->lock, &s);
    f = t->files[fd];
    t->files[fd] = NULL;
    spin_unlock_irqrestore(&t->lock, s);
    if (f)
        file_close(f);
}

/* ---- stdio ---------------------------------------------------------------------------------- */

/*
 * Stdio attaches to /dev/console once devfs has published it (the
 * tty owns echo since phase 6, so canonical reads work). Until then
 * fds stay NULL and the syscall layer falls back to its legacy raw
 * UART paths -- identical behavior to phase 5.
 *
 * The anon console vnode borrows the ops vector devfs installs for
 * character devices; its shell is immortal (floor refs = 1).
 */
extern const struct vnode_ops devfs_char_ops;

static struct vnode console_vnode;

static struct file *console_file(unsigned accmode)
{
    struct vnode *vn;
    struct char_dev *cd = char_dev_find("console");

    if (!cd)
        return NULL;

    if (!console_vnode.ops) {
        memset(&console_vnode, 0, sizeof(console_vnode));
        console_vnode.ops = &devfs_char_ops;
        console_vnode.type = V_CHARDEV;
        console_vnode.refs = 1;
    }
    console_vnode.priv = cd;

    vn = &console_vnode;
    vn_ref(vn);
    return file_alloc(vn, accmode);
}

/* ---- process hooks ------------------------------------------------------------------------------- */

int vfs_proc_fds_init(struct proc *p)
{
    static const unsigned std_amode[3] = {
        O_RDONLY, O_WRONLY, O_WRONLY
    };
    struct fd_table *t = kmalloc(sizeof(*t));

    if (!t)
        return -ENOMEM;
    memset(t, 0, sizeof(*t));
    t->lock = (spinlock_t)SPINLOCK_INIT;

    for (int i = 0; i < 3; i++) {
        struct file *std = console_file(std_amode[i]);

        if (std && vfs_fd_install(t, std) < 0)
            file_close(std);
    }

    p->fds = t;
    return 0;
}

void vfs_proc_fds_inherit(struct proc *child, const struct proc *parent)
{
    struct fd_table *ct, *pt;
    daif_state s;

    if (!parent || !parent->fds)
        return;
    pt = parent->fds;

    ct = kmalloc(sizeof(*ct));
    if (!ct)
        return;                         /* child keeps fds == NULL    */
    memset(ct, 0, sizeof(*ct));
    ct->lock = (spinlock_t)SPINLOCK_INIT;

    spin_lock_irqsave(&pt->lock, &s);
    for (int fd = 0; fd < PROC_FD_MAX; fd++)
        if (pt->files[fd])
            pt->files[fd]->refs++;      /* shared open description    */
    memcpy(ct->files, pt->files, sizeof(ct->files));
    spin_unlock_irqrestore(&pt->lock, s);

    child->fds = ct;
}

void vfs_proc_fds_release(struct proc *p)
{
    struct fd_table *t = p->fds;

    if (!t)
        return;
    p->fds = NULL;
    for (int fd = 0; fd < PROC_FD_MAX; fd++)
        vfs_fd_put(t, fd);
    kfree(t);
}
