#ifndef BLOCK_H
#define BLOCK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Block layer (phase 6): registered devices, a sector cache, a small
 * request queue, and MBR/GPT partition discovery.
 *
 * Logical sector size is fixed at 512 bytes (what virtio-blk and
 * SDHCI both speak). Driver ops receive CACHED kernel pointers; DMA
 * frontends (virtio) bounce through their own uncached staging
 * buffers so device writes stay coherent without cache maintenance.
 */

#define BLK_SECTOR_SHIFT 9
#define BLK_SECTOR_SIZE  (1u << BLK_SECTOR_SHIFT)

#define BLK_NAME_MAX     16
#define PART_MAX         24      /* per disk, incl. extended chains */
#define PART_NAME_MAX    36

struct partition {
    bool valid;
    bool is_gpt;
    bool is_extended;           /* MBR container (not listed twice) */
    uint8_t mbr_type;           /* DOS sysind byte (0 for pure GPT) */
    uint64_t lba_start;
    uint64_t lba_nsect;
    char name[PART_NAME_MAX];   /* GPT label; "" for MBR */
};

struct block_device {
    const char *name;
    void *priv;

    uint64_t capacity_sectors;
    unsigned max_sectors;       /* largest single driver transfer */

    int (*read_blocks)(struct block_device *, uint64_t lba,
                       void *buf, unsigned nsect);
    int (*write_blocks)(struct block_device *, uint64_t lba,
                        const void *buf, unsigned nsect);

    struct partition parts[PART_MAX];
    unsigned nparts;

    struct block_device *next;
};

/* ---- registry ------------------------------------------------------------------ */

#define BLK_DEV_MAX 4

int  block_register(struct block_device *bd);
struct block_device *block_find(const char *name);
struct block_device *block_first(void);
struct block_device *block_at(unsigned idx);    /* registry order */
unsigned block_device_count(void);

/* ---- buffered sector IO ------------------------------------------------------------ */

/*
 * Both take CACHED kernel pointers and any alignment; sizes need not
 * be sector-multiples (rounded up on write with zero padding).
 * Returns 0 on success.
 */
int block_read(struct block_device *bd, uint64_t lba,
               void *buf, unsigned bytes);
int block_write(struct block_device *bd, uint64_t lba,
                const void *buf, unsigned bytes);

/* ---- request queue -------------------------------------------------------------------- */

struct blk_request {
    struct block_device *bd;
    uint64_t lba;
    unsigned nsect;
    void *buf;                  /* cached */
    bool write;

    volatile bool done;
    int status;                 /* 0 = ok */
};

/*
 * Enqueue + run to completion (current drivers finish synchronously;
 * the queue exists so DMA-based completions can slot in later).
 * Returns req->status.
 */
int block_submit(struct blk_request *req);

/* ---- partitions --------------------------------------------------------------------------- */

/*
 * Detects GPT (via protective MBR) or plain MBR incl. one level of
 * EBR extended-partition chaining. Results stored on the device and
 * returned via out when non-NULL. Returns number found.
 */
int block_scan_partitions(struct block_device *bd,
                          struct partition *out /* optional mirror */);

void block_print_partitions(const struct block_device *bd);

#endif /* BLOCK_H */
