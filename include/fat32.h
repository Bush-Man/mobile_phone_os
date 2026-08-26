#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

struct block_device;

/*
 * FAT32 extras (phase 7). The filesystem itself registers as type
 * "vfat" through vfs_register_fs(); these helpers exist so board /
 * selftest code can probe raw partitions and lay down an empty
 * filesystem without userland tools.
 */

/* plausible FAT32 boot sector at (bd, lba)? 1 = yes, 0 = no */
int fat32_sniff(struct block_device *bd, uint64_t lba, uint64_t nsect);

/*
 * Format the partition window with an empty FAT32 volume (boot
 * sector + FSInfo + backup copies, two zeroed FATs, an empty root
 * directory cluster). Returns 0 or a negative errno.
 */
int fat32_mkfs(struct block_device *bd, uint64_t lba, uint64_t nsect);

void fat32_init(void);                  /* register the fs type */

#endif /* FAT32_H */
