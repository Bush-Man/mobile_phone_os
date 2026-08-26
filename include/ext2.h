#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>

struct block_device;

/*
 * ext2-lite extras (phase 7). The filesystem registers as "ext2"
 * through vfs_register_fs(); these helpers let board/selftest code
 * probe raw partitions and lay down an empty filesystem without
 * userland tools.
 *
 * Scope: rev-1 superblock, 1024-byte blocks, 128-byte inodes,
 * direct + singly/doubly/triply indirect blocks, no journaling and
 * no extended attributes -- exactly enough for a small root fs.
 */

/* plausible ext2 superblock at (bd, lba)? 1 = yes, 0 = no */
int ext2_sniff(struct block_device *bd, uint64_t lba, uint64_t nsect);

/*
 * Format the partition window as a minimal ext2 volume (one empty
 * root directory, reserved inodes 1-10 marked used, per-group
 * bitmaps/tables initialized). Returns 0 or a negative errno.
 */
int ext2_mkfs(struct block_device *bd, uint64_t lba, uint64_t nsect);

void ext2_init(void);                   /* register the fs type */

#endif /* EXT2_H */
