#ifndef ABMGR_H
#define ABMGR_H

#include <stdbool.h>
#include <stdint.h>

struct block_device;

/*
 * abmgr.h - the A/B update slot manager (phase 16, plan item 86).
 *
 * One metadata block at a fixed LBA holds a two-slot table:
 *
 *   struct ab_slot {
 *     crc            payload checksum (computed by abmgr_slot_seal)
 *     seq            monotonic image version (rollback counter)
 *     lba, nsect     payload window on the device
 *     boot_attempts  unconfirmed boots since the last confirm
 *     confirmed      1 once userspace reported a healthy boot
 *     active         1 for the slot the bootloader would load
 *   }
 *
 * Policy:
 *   - a slot becomes active only via abmgr_switch() and only if it
 *     is sealed (valid CRC);
 *   - the active slot must reach abmgr_confirm() within
 *     AB_MAX_ATTEMPTS unconfirmed boots; abmgr_evaluate() otherwise
 *     rolls back to the other valid slot (falling back to the
 *     highest-seq one) and bumps the rollback counter;
 *   - seq never decreases on a switch, so a rolled-back slot keeps
 *     its version identity and the counter is monotonically
 *     increasing across the device.
 *
 * The QEMU dev image keeps the table in the tail sectors of the
 * scratch disk; the release layout (docs/RELEASE.md) gives each
 * slot its own partition.
 */

#define AB_MAGIC         0x41424d47u    /* "ABMG"                   */
#define AB_TABLE_VERSION 1u
#define AB_SLOTS         2u
#define AB_MAX_ATTEMPTS  3u

struct ab_slot {
    uint32_t crc;                   /* payload checksum            */
    uint32_t seq;                   /* image version, monotonic    */
    uint64_t lba;                   /* payload window              */
    uint64_t nsect;
    uint8_t  boot_attempts;
    uint8_t  confirmed;
    uint8_t  active;
    uint8_t  valid;                 /* sealed with a verified CRC  */
};

struct ab_table {
    uint32_t        magic;
    uint32_t        version;
    struct ab_slot  slots[AB_SLOTS];
    uint32_t        rollbacks;      /* automatic rollbacks so far  */
    uint32_t        crc;            /* CRC of everything above     */
};

struct ab_stats {
    int      active;                /* -1 while no slot is active  */
    uint32_t rollbacks;
    uint32_t confirms;
    uint32_t switches;
};

/* attach to a device + table LBA; formats the table when absent   */
int  abmgr_attach(struct block_device *bd, uint64_t table_lba);

int  abmgr_active(void);                /* slot idx or -1              */

/* compute + store the CRC of a slot's payload; marks it valid     */
int  abmgr_slot_seal(unsigned idx, uint64_t lba, uint64_t nsect,
                     uint32_t seq);

/* boot lifecycle                                                   */
int  abmgr_boot_begin(void);            /* ++attempts on active        */
int  abmgr_confirm(void);               /* healthy boot reported       */
int  abmgr_switch(void);                /* activate the other slot     */
int  abmgr_evaluate(void);              /* 1 = rolled back, 0 = ok,
                                           <0 = errno                  */

/* direct table access for selftests (snapshot under the hood)     */
int  abmgr_table_get(struct ab_table *out);
int  abmgr_table_put(const struct ab_table *in);

struct block_device *abmgr_device(void);
uint64_t abmgr_table_lba(void);

#endif /* ABMGR_H */
