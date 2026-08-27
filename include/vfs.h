#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lib.h"                /* strlen for ux_dirent_len below  */
#include "spinlock.h"

struct block_device;
struct proc;

/*
 * vfs - virtual filesystem switch (phase 7).
 *
 * Deliberately "inodes-lite": there is no separate dentry cache and
 * no on-memory inode hash. A vnode is a small refcounted shell that
 * carries an ops vector plus fs-private state; directories are
 * resolved by asking the parent to look names up every time, which
 * is cheap at this scale (dirs here hold dozens of entries, not
 * thousands).
 *
 * Layering:
 *
 *   syscalls (kernel/syscall.c)
 *      -> fd table / struct file      (this header, fs/vfs.c)
 *         -> path resolution + mount tree
 *            -> per-fs ops (ramfs, devfs, fat32, ext2)
 *               -> block layer / chardev registry  (phase 6)
 *
 * One flat namespace: mounts are keyed by absolute mountpoint path
 * and resolution crosses them by string comparison while descending.
 */

#define VFS_NAME_MAX    64      /* longest single component           */
#define VFS_PATH_MAX   160      /* longest absolute path              */
#define VFS_MOUNT_MAX    8      /* simultaneous mounts                */
#define PROC_FD_MAX     16      /* fds per process                    */

/* open(2)-style flags; values match Linux/generic ABI */
#define O_RDONLY    0u
#define O_WRONLY    1u
#define O_RDWR      2u
#define O_ACCMODE   3u
#define O_CREAT   0x040u
#define O_EXCL    0x080u
#define O_TRUNC   0x200u
#define O_APPEND  0x400u

/* lseek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* d_type values (Linux DT_* numbers) */
#define DT_UNKNOWN 0u
#define DT_FIFO    1u
#define DT_CHR     2u
#define DT_DIR     4u
#define DT_BLK     6u
#define DT_REG     8u
#define DT_LNK     10u

enum vtype {
    V_NONE = 0,
    V_FILE,
    V_DIR,
    V_CHARDEV,
    V_BLOCKDEV,
    /* phase 8: anonymous IPC nodes (never reachable via path)     */
    V_PIPE,                     /* one end of a pipe                  */
    V_SOCK,                     /* unix-domain socket endpoint        */
};

struct vnode;
struct mount;

/* attribute snapshot for getattr() */
struct vattr {
    enum vtype type;
    uint64_t size;                  /* files only; dirs report 0      */
};

/*
 * Per-fs operations. Any member may be NULL where meaningless
 * (e.g. create on devfs); the wrappers in fs/vfs.c check first.
 *
 * readdir is index-based: successive calls with idx = 0,1,2,... must
 * return the idx-th VALID entry of the directory (deleted/holes
 * skipped internally); -ENOENT once past the end.
 */
struct vnode_ops {
    int (*lookup)(struct vnode *dir, const char *name,
                  struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, enum vtype t,
                  struct vnode **out);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*readdir)(struct vnode *dir, unsigned idx,
                   char *name_out, uint8_t *type_out);
    long (*read)(struct vnode *vn, uint64_t off,
                 void *buf, size_t len);
    long (*write)(struct vnode *vn, uint64_t off,
                  const void *buf, size_t len);
    int (*getattr)(struct vnode *vn, struct vattr *out);

    /*
     * Phase 8 (poll multiplexing): report currently-true readiness
     * conditions as a POLLIN/POLLOUT/POLLHUP bitmask without ever
     * blocking. Optional; the poll syscall treats absent ->poll as
     * "always ready both ways" so regular files never stall it.
     */
    unsigned (*poll)(struct vnode *vn);

    /* called by vn_unref when the last reference drops */
    void (*destroy)(struct vnode *vn);
};

struct vnode {
    const struct vnode_ops *ops;
    enum vtype type;
    volatile unsigned refs;
    struct mount *mp;               /* owning mount, NULL for anon    */
    void *priv;                     /* fs-private                     */
    uint64_t ino;                   /* informational                  */
};

/* one mounted filesystem instance */
struct mount {
    const char *fstype;
    struct block_device *bd;        /* NULL for memory filesystems    */
    uint64_t part_lba;              /* partition window inside bd     */
    uint64_t part_nsect;
    struct vnode *root;             /* ref held for the mount lifetime*/
    bool active;
    char path[VFS_PATH_MAX];        /* mountpoint in the namespace    */
    struct mount *next;
};

/* open file description: shared across dup()/fork() via ->refs     */
struct file {
    struct vnode *vn;
    uint64_t off;                   /* data offset or dir cookie      */
    unsigned flags;                 /* open flags                     */
    volatile unsigned refs;
};

/* per-process open-file table (owned by struct proc)               */
struct fd_table {
    spinlock_t lock;
    struct file *files[PROC_FD_MAX];
};

/*
 * getdents record layout (our own compact ABI): a u16 total record
 * length (name+NUL included, padded to 8) followed by d_type and the
 * NUL-terminated name. Records are packed back to back.
 */
#define UX_DIRENT_ALIGN 8

static inline size_t ux_dirent_len(const char *name)
{
    size_t need = 3 + strlen(name) + 1;     /* len + type + NUL       */

    return (need + UX_DIRENT_ALIGN - 1) & ~(UX_DIRENT_ALIGN - 1);
}

/*
 * Phase 8 poll multiplexing ABI: SYS_poll takes an array of these
 * (8 bytes each, packed), reads ->want and fills ->got for every
 * READY descriptor before returning the ready count. Compact on
 * purpose: only ready entries are written back (a libc can expand
 * this into POSIX poll semantics later).
 */
struct uxpollfd {
    int32_t  fd;
    uint16_t want;
    uint16_t got;
} __attribute__((packed));

/* ---- subsystem + mounts ------------------------------------------------- */

typedef int (*fs_mount_fn)(struct mount *m);

void vfs_subsys_init(void);                 /* registries + root slot */
int  vfs_register_fs(const char *fstype, fs_mount_fn mount_fn);

/*
 * Mount `fstype` at `path` (absolute). For disk filesystems pass the
 * block device and partition window; memory filesystems take NULL/0.
 * Returns 0 or a negative errno.
 */
int vfs_mount(const char *fstype, const char *path,
              struct block_device *bd, uint64_t lba, uint64_t nsect);

struct vnode *vfs_root(void);               /* namespace root, ref'd  */
bool vfs_path_is_mounted(const char *path); /* debug/report helper    */
unsigned vfs_mount_count(void);

/* ---- path resolution ------------------------------------------------------ */

/*
 * Resolve an absolute path to a ref'd vnode ("." and ".." normalize
 * lexically with clamping at "/"; mountpoints crossed on descent).
 */
int  vfs_resolve(const char *path, struct vnode **out);

int  vfs_lookup_at(struct vnode *dir, const char *name,
                   struct vnode **out);
int  vfs_create_at(struct vnode *dir, const char *name, enum vtype t,
                   struct vnode **out);
int  vfs_unlink_at(struct vnode *dir, const char *name);
int  vfs_readdir_at(struct vnode *dir, unsigned idx,
                    char *name_out, uint8_t *type_out);

/* whole-path convenience wrappers used by the syscall layer */
int  vfs_open(const char *path, unsigned flags, struct file **out);
int  vfs_unlink(const char *path);
int  vfs_mkdir(const char *path);
int  vfs_rmdir(const char *path);

/* ---- refs ------------------------------------------------------------------ */

void vn_ref(struct vnode *vn);
void vn_unref(struct vnode *vn);

/* op dispatch with NULL checks; return -EINVAL/-ENOTDIR/-EISDIR etc */
int  vfs_getattr(struct vnode *vn, struct vattr *out);

/* ---- files + fd tables -------------------------------------------------------- */

struct file *file_alloc(struct vnode *vn, unsigned flags);
void file_close(struct file *f);    /* drop one reference             */

long f_read(struct file *f, void *buf, size_t len);
long f_write(struct file *f, const void *buf, size_t len);
int  f_lseek(struct file *f, int64_t off, int whence, int64_t *out);
long f_getdents(struct file *f, void *kbuf, size_t buflen);

/*
 * Process lifecycle hooks (called from kernel/proc.c):
 *   init    - fresh table with stdio attached to /dev/console
 *   inherit - fork: share parent's struct file references
 *   release - reap: close everything and free the table
 * All tolerate p->fds == NULL so early boot stays simple.
 */
int  vfs_proc_fds_init(struct proc *p);
void vfs_proc_fds_inherit(struct proc *child, const struct proc *parent);
void vfs_proc_fds_release(struct proc *p);

int  vfs_fd_install(struct fd_table *t, struct file *f);
struct file *vfs_fd_get(struct fd_table *t, int fd);    /* refs++   */
void vfs_fd_put(struct fd_table *t, int fd);            /* unassign */

#endif /* VFS_H */