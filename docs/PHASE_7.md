# Phase 7 — Filesystems (Implementation Log)

Milestone scope reached: a VFS with a mount tree, refcounted vnodes
("inodes-lite", no dentry cache), file ops vectors and per-process fd
tables; an in-memory ramfs as the early-boot root; a devfs whose
directory listing IS the phase-6 device registries; FAT32 with full
read/write support (LFN, cluster allocation, both FAT mirrors kept in
sync) plus mkfs/probe helpers; ext2-lite with read/write (direct +
singly/doubly/triply indirect), bitmap allocators, directory
insert/remove, no journaling, plus its own mkfs; and the syscall
surface (open/close/read/write/lseek/getdents/mkdir/rmdir/unlink)
routed through per-process fd tables inherited across fork. Per the
standing coordination decision, final integration builds/QEMU smoke
runs are deferred to the project-complete point; every new/edited
translation unit was kept compile-clean via per-file
`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Wextra -ffreestanding
-mgeneral-regs-only` checks after each batch (zero warnings/errors at
commit time for all phase-7 files).

## Order of files written

### Batch A — string helpers + VFS core

1. **`include/lib.h` / `lib/string.c`** — closed the "lib has no
   str* routines" gap the same way phase 6 closed memcmp: added
   freestanding `strlen`, `strcmp`, `strncmp`, and bounded
   `kstrlcpy` (always NUL-terminates, returns src length so callers
   detect truncation).
2. **`include/vfs.h`** — the whole data-type layer in one pass:
   limits (`VFS_NAME_MAX 64`, `VFS_PATH_MAX 160`, `VFS_MOUNT_MAX 8`,
   `PROC_FD_MAX 16`); Linux-valued open flags/DT_*/SEEK_*; enum
   vtype (V_FILE/V_DIR/V_CHARDEV/V_BLOCKDEV); `struct vnode_ops`
   (lookup/create/unlink/readdir/read/write/getattr + destroy hook);
   refcounted `struct vnode` shell; `struct mount` (fstype, block
   device + partition window, root vnode ref, mountpoint path);
   `struct file` (shared open description, off doubles as dir
   cookie) and `struct fd_table`; compact getdents record layout
   (`ux_dirent_len`: u16 reclen + u8 type + NUL name, padded to 8);
   fs-type registry + mount API, path resolution API, file/fd APIs,
   and the process lifecycle hooks.
3. **`include/syscall.h`** — errno set extended to what VFS needs
   (EIO/EEXIST/ENODEV/ENOTDIR/EISDIR/EBUSY/EMFILE/ENOSPC/ESPIPE/
   EROFS/ENAMETOOLONG/ENOTEMPTY, Linux values). Syscall numbers came
   later in batch F.
4. **`fs/vfs.c`** — one global irq-safe lock guards registries,
   mount table and ALL refcounts (no fs call ever runs under it);
   `struct fs_type` registry (`vfs_register_fs`); static mount
   slots where slot 0 is always "/"; `vfs_mount` claims the slot,
   calls the fs mount fn, rolls back on failure; lexical path
   normalization ("."/".." clamped at root, max 24 components) then
   a linear walk from the root that crosses mountpoints by string
   comparison of the descending prefix (`path_join` helper keeps
   cur_path canonical, "" == root); `vfs_open` with O_CREAT split
   into parent+leaf (bad_leaf_name rejects "."/".."), O_EXCL,
   O_TRUNC via the write(vn,0,NULL,0) idiom; files: `file_alloc`
   consumes a vnode reference, `f_read/f_write` enforce accmode +
   O_APPEND, `f_lseek` (dir -> -EISDIR), `f_getdents` packs records
   and only consumes the dir cookie when a record actually fits;
   fd tables (`vfs_fd_install/get/put`, refs++ on get so a racing
   close can't strand a pointer); stdio: anon immortal console
   vnode borrowing devfs's ops vector, attached as fds 0-2 when the
   console chardev exists; process hooks `vfs_proc_fds_init /
   _inherit (shared descriptions via refs++) / _release`.

> Commit: `Add VFS core: mount tree, name resolution and per-process fd tables (phase 7)`

### Batch B — ramfs

5. **`fs/ramfs.c`** — nodes hold their own names in sibling lists;
   file payload is ONE growable kmalloc buffer (doubling up to 64
   KiB steps, hard cap 256 KiB to protect the heap); lifetime model:
   `vn_live` counts vnode shells per node, unlink detaches +
   orphans, storage dies either immediately or in `destroy()` when
   the last shell drops (open fds survive unlink, POSIX-style);
   readdir walks the child list by index; writes zero-fill holes.
   Registers fs type `"ramfs"`.

> Commit: `Add ramfs in-memory filesystem for the early-boot root (phase 7)`

### Batch C — devfs

6. **`include/chardev.h` / `drivers/chardev.c`** — additive indexed
   accessor `char_dev_at(idx)` mirroring the block registry's
   `block_at()`.
7. **`fs/devfs.c`** — synthetic filesystem over the phase-6
   registries: char devices appear as V_CHARDEV nodes, block
   devices as V_BLOCKDEV nodes, generated on demand in lookup/
   readdir (index space = chars first, then blocks; shells are
   heap-only and freed by destroy). Char IO passes straight through
   the registry ops (so `/dev/console` speaks tty line discipline);
   block IO is byte-offset over `block_read/block_write` bouncing
   through one sector buffer so any alignment works. create/unlink
   are NULL (-EPERM semantics): the namespace is registry-owned.
   One shared ops table (`devfs_char_ops`) also serves the anon
   console vnode from vfs.c. Root dir is a static immortal vnode.

> Commit: `Add devfs device-node filesystem over the char/block registries (phase 7)`

### Batch D — FAT32

8. **`include/fat32.h` / `fs/fat32.c`** — full read/write driver:
   BPB parse with sanity gates (512-byte sectors, power-of-two
   cluster size <= 128, 1-2 FATs, plausible root cluster, >= 4085
   clusters); FAT entry access mirrored across BOTH copies on every
   mutation; chain walk/seek cursor, grow (scan-for-free, zero-fill
   before linking, EOC stamp) and free; directories addressed by
   PHYSICAL slot index across the chain (32 B slots); LFN collect
   on read (head = ord|0x40 restarts the accumulator, checksum
   verified against the SFN, ASCII decode capped at VFS_NAME_MAX)
   and LFN emit on create (fragments written head-first physically,
   descending ords toward the SFN slot); name matching prefers the
   long name, falls back to case-insensitive decoded-SFN compare;
   SFN generation with numeric tails (~1..~9) only when the name is
   not exact-uppercase-8.3; create allocates a first cluster for
   directories immediately and writes "."/".." (parent cluster
   recorded); slot allocation finds runs of free/deleted slots and
   grows directories cluster-wise; unlink frees file chains,
   refuses non-empty dirs, tombstones the SFN AND its LFN run
   (0xE5 backwards); truncate frees the chain and clears the dirent
   via stored de_lba/de_off location (`dirent_sync` rewrites
   first-cluster + size fields in place).
   Locking: virtio block IO sleeps, so a spinlock would deadlock —
   each instance carries a SLEEPING big-fs-lock (test-and-set flag +
   phase-4 `wait_sleep_when` queue); every vnode op wraps itself,
   internal helpers assume held, nothing recurses (hole-filling was
   split out of fat_write for exactly that reason).
   Helpers: `fat32_sniff` (BPB plausibility vs partition window)
   and `fat32_mkfs` (boot sector + FSInfo + backup copies at 6/7,
   two zeroed FATs with reserved entries + root-cluster EOC,
   zeroed single-cluster root; fatsz solved so clusters >= 65525 —
   an honest FAT32 volume). Registers fs type `"vfat"`.

> Commit: `Add FAT32 read/write filesystem with LFN, mkfs and probe helpers (phase 7)`

### Batch E — ext2-lite

9. **`include/ext2.h` / `fs/ext2.c`** — rev-1 superblock, 1024-byte
   blocks (two sectors), first_data_block = 1, blocks_per_group =
   8192 (one bitmap block/group), 2048 inodes/group x 128 bytes,
   groups described by the on-disk GDT which mount() trusts like a
   real driver (entries straddle sectors: gather/scatter through
   sector bounce buffers everywhere inode/GDT records are touched).
   Block mapping is a recursive walker: subtree root pointer +
   level + index; freshly allocated roots are reported to the OWNER
   of the pointer slot (indirect-block buffer or in-memory inode)
   via a changed flag so persistence stays in exactly one place;
   holes return 0 without allocating. Free walks mirror the same
   recursion (data + indirect metadata). Bitmap allocators scan
   group by group, never hand out bits beyond the volume end;
   ialloc skips reserved inodes 1..10. Directory entries handled at
   absolute byte offsets: add splits live-record slack or reuses
   tombstones spanning enough space, else appends a fresh block
   (rec_len = blocksize); remove merges length into the PREVIOUS
   record or leaves an explicit ino=0 tombstone when opening a
   block; mkdir writes . / .. and bumps parent links.
   Same sleeping-big-fs-lock pattern as fat32 (documented
   duplication until phase 8 brings real mutexes).
   Helpers: `ext2_sniff` (magic + geometry sanity vs window) and
   `ext2_mkfs` (per-group layout, zeroed bitmaps/tables, GDT RMW,
   reserved-inode bits, single-block root dir, superblock written
   LAST; deliberately stops 32 sectors short of the window end --
   see design decisions). Registers fs type `"ext2"`.

> Commit: `Add ext2-lite read/write filesystem with mkfs and probe helpers (phase 7)`

### Batch F — syscalls + process integration

10. **`include/syscall.h`** — SYS_open 13, SYS_close 14, SYS_lseek
    15, SYS_getdents 16, SYS_mkdir 17, SYS_rmdir 18, SYS_unlink 19
    (continuing the phase-5 numbering).
11. **`kernel/syscall.c`** — read/write rewritten onto the fd
    table: chunked uaccess copy loops around f_read/f_write, short-
    write aware; fds 0-2 are the stdio opened at spawn (tty-backed:
    reads now BLOCK for a canonical line instead of draining the
    raw RX ring -- the phase-6 log called for this handover).
    Legacy fallback preserved verbatim when p->fds == NULL (boot
    images without devfs keep byte-exact phase-5 behavior). New
    syscalls: open (path copied in via strnlen+copy, late fd-table
    creation tolerated), close (drops the get-ref AND the slot),
    lseek (64-bit offset, returns position), getdents (fills our
    packed ux_dirent records; 0 = end of directory), mkdir/rmdir/
    unlink (path wrappers over the VFS).
12. **`include/proc.h`** — `struct proc` grew `struct fd_table *fds`
    (NULL = kernel thread / not set up).
13. **`kernel/proc.c`** — three minimal hooks in the existing
    lifecycle: spawn allocates a fresh table (stdio attached) with
    full cleanup on later failure paths; fork inherits via
    `vfs_proc_fds_inherit` (POSIX dup semantics: shared open
    descriptions, separate offsets? NO -- offsets are IN the shared
    description, matching Unix); reap releases everything before
    the task slot is freed. Phase-5-owned code untouched beyond
    these marked call sites.

> Commit: `Wire fd-based read/write plus open/close/lseek/getdents/mkdir/rmdir/unlink syscalls into processes (phase 7)`

### Batch G — bring-up + selftest + demo wiring

14. **`include/ramfs.h` / `include/devfs.h`** — one-line init
    prototypes mirroring `fat32.h`/`ext2.h` style.
15. **`kernel/phase7.c`** — `phase7_init(&plat)` called from kmain
    right after phase6_init: registers the four fs types, mounts
    ramfs "/" (panic if it fails -- nothing else works without a
    root) and devfs "/dev" (both pure registry walks, safe in boot
    context), reports the mount count, spawns the fstest task.
    Disk-backed mounts are NOT done here: they need blocking IO,
    same boot-context rule phase 6 established.
16. **`kernel/selftest_fs.c`** — the fstest battery:
    - ramfs: mkdir/create/write/read-back/lseek(END)+partial-read/
      getdents enumeration/unlink/rmdir.
    - devfs: /dev/console writable (proves the phase-6 registry ->
      VFS bridge), /dev/nope -> ENOENT.
    - disk negotiation (`ensure_layout`): handles three histories —
      virgin disk, phase-6's single-partition MBR (interior blank
      check), or our own earlier dual layout; writes a dual MBR
      (FAT32 type 0x0B @ LBA 2048, 36 MiB; ext2 type 0x83 covering
      the rest) and rescans. Converges regardless of which of
      drvtest/fstest formatted first, because both orders end at
      the dual layout and drvtest respects pre-existing tables.
    - per-fs battery (vfat mounted at /dos, ext2 at /ext2):
      sniff -> mkfs when unformatted -> mount -> PERSISTENCE
      COUNTER (read decimal counter, unlink, rewrite n+1; printed
      value increasing across reboots IS the milestone proof) ->
      64 KiB pattern file round-trip (96+ FAT clusters / 60+ ext2
      blocks, i.e. chain growth AND singly-indirect mapping) ->
      unlink; vfat additionally exercises LFN create/read/delete
      ("Long File Name.txt"); ext2 additionally mkdir/dup-EEXIST/
      rmdir.
    Summary line `selftest: fs ok` matches the harness grep style.
17. **`kernel/main.c`** — extern decl + the marked phase-7 call
    site after phase6_init (kept minimal per the standing
    convention).
18. **`Makefile`** — `fs/*.c` added to the SRCS_C wildcard; no
    other changes.

> Commit: `Wire phase 7 filesystem bring-up, dual-layout disk setup and persistence selftest`

(One follow-up fix commit: missing `#include "panic.h"` in
phase7.c caught by the final sweep.)

## Design decisions worth keeping

- **Sleeping fs locks, not spinlocks.** The virtio block path parks
  via msleep(); holding a spinlock across it would deadlock the
  machine. Each fat/ext2 instance carries a test-and-set flag plus
  a phase-4 waitqueue; vnode ops acquire/release, internals assume
  held. The pattern is duplicated between fat32.c and ext2.c until
  phase 8 introduces real mutexes -- deliberate, documented debt.
- **Vnode shells are dumb.** All smarts live in fs-private structs;
  the shell exists for uniform refcounting + ops dispatch. Refcounts
  live under the global vfs lock; fs state lives under the fs lock;
  never nested the other way.
- **Physical-slot directory addressing (FAT).** Slots are indexed
  across the whole cluster chain, deleted entries stay invisible to
  iteration but reusable for placement; LFN runs are collected
  keyed by ordinal so fragments may arrive in any order.
- **Recursive bmap with owner-persisted patches (ext2).** The
  walker never writes pointers itself; it reports "I allocated a
  root" upward so the inode OR the parent indirect block performs
  exactly one authoritative store. No stale-buffer hazards.
- **Dual-layout disk negotiation.** The selftest owns formatting on
  blank disks but respects any pre-existing valid table; adopting
  phase-6's leftover blank partition (rewrite into dual layout)
  makes `make run-disk` converge to a state where BOTH filesystems
  get exercised on every image history.
- **ext2 stops 32 sectors early.** The phase-6 block selftest parks
  pattern writes at the very tail of the disk EVERY boot; the
  filesystem's usable block count excludes that margin so the two
  selftests can never corrupt each other.
- **Stdio falls back gracefully.** No devfs/no console -> fds NULL
  -> legacy raw-UART read/write paths preserved byte-for-byte, so
  phase-5-era user binaries behave identically even if the tty
  takeover never happened.
- **getdents is ours.** Compact {u16 len, u8 type, name[]} records
  padded to 8; 0 return = end-of-dir; cookie not consumed when the
  next record would overflow the caller buffer. A libc can wrap
  this into POSIX readdir later.

## Verification status

Per coordination decision (builds/QEMU smoke runs deferred until
the project-complete point), no `make` target was run this phase.
Every phase-7 translation unit was checked with
`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Wextra -ffreestanding
-fno-builtin -mgeneral-regs-only` after each batch; final sweep
results:

```
CLEAN lib/string.c        CLEAN fs/vfs.c          CLEAN fs/ramfs.c
CLEAN fs/devfs.c          CLEAN fs/fat32.c        CLEAN fs/ext2.c
CLEAN kernel/phase7.c     CLEAN kernel/selftest_fs.c
CLEAN kernel/main.c       CLEAN drivers/chardev.c
pre-existing (phase-5 owned, untouched):
  kernel/syscall.c  execve uacc_strnlen casts (HEAD-identical)
  kernel/proc.c     proc_user_fault %llx cast (HEAD-identical)
```

When integration lands:

```
make run-disk     # boots with virtio-blk; expect:
                  #   vfs mounts: ramfs / , devfs /dev
                  #   fstest ramfs/devfs PASS lines
                  #   dual-layout MBR adoption on first run
                  #   vfat mkfs once, then persist counter increments
                  #   ext2 ditto; selftest: fs ok
make run && reboot (re-run)   # counters must have grown by 1 each
make test         # unchanged harness; expected-phase argv still
                  # owned by whichever stream integrates last
```

Notes carried forward:

- Mounting happens inside fstest; processes spawning before that
  see only / and /dev (no /dos, /ext2 yet). A proper mount-on-boot
  service belongs to the phase-14 init work.
- Unmount is a deliberate non-goal (memory filesystems never
  unmount; disk mounts live for the session).
- FAT FSInfo free counts stay "unknown" (0xFFFFFFFF); we never
  trust them on read either.
- ext2: no journal, no extended attributes, no fragment support,
  superblock not backed up; sb_parse rejects anything outside the
  mkfs geometry rather than approximating.
- Signals do not interrupt blocking tty reads yet (EINTR machinery
  still owed from phase 5, unchanged here).
- exec-time close-on-exec / fd inheritance limits remain simple
  (all fds inherit; PROC_FD_MAX caps the table).
