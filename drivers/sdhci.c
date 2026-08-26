/*
 * sdhci.c - standard SD Host Controller (SDHCI) backend + SD memory
 * card protocol, PIO + polling.
 *
 * Register map follows the SD Host Controller Simplified Spec:
 * byte/word-addressable registers are accessed at native width.
 * The card path implements the classic bring-up: CMD0 -> CMD8 ->
 * ACMD41(HCS) -> CMD2 -> CMD3 -> CMD9 -> CMD7 -> ACMD6(4-bit) ->
 * CMD16, then single-block reads (CMD17) and writes (CMD24 +
 * CMD13 polling for the programming state).
 *
 * Registered as a block_device with max_sectors=1; the block layer's
 * cache and chunking make that transparent to consumers.
 */

#include <stdint.h>
#include <stddef.h>

#include "block.h"
#include "lib.h"
#include "mm/kheap.h"
#include "mm/types.h"
#include "mmc.h"
#include "mmio.h"
#include "spinlock.h"

/* ---- register accessors (device memory, native widths) ------------------------ */

static inline uint8_t  r8(uintptr_t a)  { return *(volatile uint8_t *)a; }
static inline uint16_t r16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline uint32_t r32(uintptr_t a) { return mmio_read32(a); }

static inline void w8(uintptr_t a, uint8_t v)
{
    *(volatile uint8_t *)a = v;
}

static inline void w16(uintptr_t a, uint16_t v)
{
    *(volatile uint16_t *)a = v;
}

static inline void w32(uintptr_t a, uint32_t v)
{
    mmio_write32(a, v);
}

/* ---- SDHCI registers ------------------------------------------------------------ */

#define SD_DMA         0x00
#define SD_BLK_SIZE    0x04
#define SD_BLK_COUNT   0x06
#define SD_ARGUMENT    0x08
#define SD_XFER_MODE   0x0c
#define SD_COMMAND     0x0e
#define SD_RESP        0x10            /* ..0x1f */
#define SD_BUFFER      0x20
#define SD_PSTATE      0x24
#define SD_HOST_CTRL   0x28
#define SD_POWER       0x29
#define SD_CLOCK       0x2c
#define SD_TIMEOUT     0x2e
#define SD_RESET       0x2f
#define SD_NINT        0x30            /* normal int status (16-bit) */
#define SD_EINT        0x32            /* error int status */
#define SD_NINT_EN     0x34
#define SD_EINT_EN     0x36
#define SD_NINT_SIG    0x38
#define SD_CAP0        0x40

#define PSTATE_CMD_INH   0x001u
#define PSTATE_DAT_INH   0x002u
#define PSTATE_WR_ACTIVE 0x100u
#define PSTATE_RD_ACTIVE 0x200u
#define PSTATE_BUF_WR    0x400u
#define PSTATE_BUF_RD    0x800u

#define HOST_4BIT      0x02u           /* note: bit1 in the spec */

#define CLK_INT_EN     0x0001u
#define CLK_STABLE     0x0002u
#define CLK_SD_EN      0x0004u
#define CLK_DIV_SHIFT  6
#define CLK_DIV_MASK   0x03ffu

#define RST_ALL        0x01u
#define RST_CMD        0x02u
#define RST_DAT        0x04u

#define NIS_CMD_DONE   0x0001u
#define NIS_XFER_DONE  0x0002u
#define NIS_BUF_WR     0x0010u
#define NIS_BUF_RD     0x0020u
#define NIS_ERR        0x8000u

/* command flag encodings */
#define RESP_NONE      0x00u
#define RESP_R136      (0x01u << 4)    /* long: CID/CSD */
#define RESP_R48       (0x02u << 4)
#define RESP_R48B      (0x03u << 4)    /* busy: R1b */
#define CMD_CRC        0x08u
#define CMD_IDX_CHK    0x10u
#define CMD_DATA       0x20u

/* transfer mode bits */
#define TM_BCEN        0x0002u
#define TM_MULTI       0x0020u
#define TM_READ        0x0010u

/* ---- MMC command numbers ----------------------------------------------------------- */

#define CMD0_GO_IDLE   0
#define CMD2_ALL_CID   2
#define CMD3_REL_ADDR  3
#define CMD7_SELECT    7
#define CMD8_IF_COND   8
#define CMD9_SEND_CSD  9
#define CMD12_STOP     12
#define CMD13_STATUS   13
#define CMD16_SET_LEN  16
#define CMD17_READ     17
#define CMD24_WRITE    24
#define CMD55_APP      55
#define ACMD41_SD_SEND_OP 41

/* ---- device state ---------------------------------------------------------------------- */

struct sdhci_host {
    uintptr_t base;
    unsigned base_clock_mhz;
    bool sdhc_block_addr;               /* OCR[30]: card uses LBA addressing */
    uint32_t rca;

    struct block_device bd;
    char name[8];

    spinlock_t lock;
};

static struct sdhci_host *s_hosts[2];
static unsigned s_nhosts;

/* ---- low-level helpers --------------------------------------------------------------------- */

static void sd_reset(struct sdhci_host *h, uint8_t mask)
{
    uintptr_t b = h->base;

    w8(b + SD_RESET, mask);
    for (unsigned t = 0; t < 100000 && (r8(b + SD_RESET) & mask); t++)
        ;
}

static void sd_set_clock(struct sdhci_host *h, unsigned khz)
{
    uintptr_t b = h->base;
    unsigned div, enc;
    uint16_t ctl;

    if (!h->base_clock_mhz)
        h->base_clock_mhz = r8(b + SD_CAP0);
    if (!h->base_clock_mhz)
        h->base_clock_mhz = 50;         /* sane default */

    /* stop the SD clock while reprogramming */
    ctl = (uint16_t)(r16(b + SD_CLOCK) & ~CLK_SD_EN);
    w16(b + SD_CLOCK, ctl);

    /*
     * Divider field holds div/2 where div = base/(2*target) rounded
     * up to an even value; 0 means bypass.
     */
    {
        unsigned base = h->base_clock_mhz * 1000u;
        unsigned want = 2 * khz;

        div = (base + want - 1) / want;
        if (div > 256)
            div = 256;
        if (div < 2)
            div = 0;
        else
            div -= div & 1;             /* even only */
        enc = div / 2u;
    }

    ctl = (uint16_t)(CLK_INT_EN |
                     ((enc & CLK_DIV_MASK) << CLK_DIV_SHIFT));
    w16(b + SD_CLOCK, ctl);

    for (unsigned t = 0; t < 1000000 && !(r16(b + SD_CLOCK) & CLK_STABLE); t++)
        ;

    w16(b + SD_CLOCK,
        (uint16_t)(ctl | CLK_STABLE | CLK_SD_EN));
}

/* ---- command engine -------------------------------------------------------------------------- */

/* returns 0 ok; fills resp[0..3] when a response is expected */
static int sd_cmd_raw(struct sdhci_host *h, unsigned idx, uint32_t arg,
                      unsigned resp_flags, bool data_read, uint32_t resp[4])
{
    uintptr_t b = h->base;
    uint32_t pstate, cmd = ((idx & 0x3fu) << 8) | resp_flags;
    int r = -1;

    /* wait out any previous activity */
    for (unsigned t = 0; t < 1000000; t++) {
        pstate = r32(b + SD_PSTATE);
        if (!(pstate & PSTATE_CMD_INH))
            break;
    }
    for (unsigned t = 0; t < 1000000; t++) {
        pstate = r32(b + SD_PSTATE);
        if (!(pstate & PSTATE_DAT_INH))
            break;
    }

    w32(b + SD_ARGUMENT, arg);
    w16(b + SD_COMMAND, (uint16_t)cmd);

    for (unsigned t = 0; t < 1000000; t++) {
        uint16_t nis = r16(b + SD_NINT);

        if (nis & NIS_ERR) {
            (void)r16(b + SD_EINT);     /* read clears error bits */
            w16(b + SD_NINT, NIS_ERR);
            goto out;
        }
        if (nis & NIS_CMD_DONE) {
            w16(b + SD_NINT, NIS_CMD_DONE);
            r = 0;
            break;
        }
    }

    if (r == 0 && resp)
        for (int i = 0; i < 4; i++)
            resp[i] = r32(b + SD_RESP + 4 * i);

    (void)data_read;
out:
    return r;
}

/* R1 helper: response only */
static int sd_cmd_r1(struct sdhci_host *h, unsigned idx, uint32_t arg,
                     uint32_t *r1_out)
{
    uint32_t resp[4] = { 0 };
    int r = sd_cmd_raw(h, idx, arg, RESP_R48 | CMD_CRC | CMD_IDX_CHK,
                       false, resp);

    if (r == 0 && r1_out)
        *r1_out = resp[0];
    return r;
}

/* ACMD wrapper: CMD55 then the app command */
static int sd_acmd(struct sdhci_host *h, unsigned idx, uint32_t arg,
                   unsigned resp_flags, uint32_t resp[4])
{
    uint32_t rca_arg = h->rca << 16;
    int r = sd_cmd_raw(h, CMD55_APP, rca_arg, RESP_R48 | CMD_CRC | CMD_IDX_CHK,
                       false, NULL);

    if (r != 0)
        return r;
    return sd_cmd_raw(h, idx, arg, resp_flags, false, resp);
}

/* ---- single-block data transfers (PIO) ---------------------------------------------- */

static int sd_read_block_pio(struct sdhci_host *h, uint64_t lba, void *buf)
{
    uintptr_t b = h->base;
    uint64_t addr = h->sdhc_block_addr ? lba : lba * 512u;
    uint32_t *out = buf;

    w16(b + SD_BLK_SIZE, 512);
    w16(b + SD_BLK_COUNT, 1);
    w16(b + SD_XFER_MODE, TM_BCEN | TM_READ);

    if (sd_cmd_raw(h, CMD17_READ, (uint32_t)addr,
                   RESP_R48 | CMD_CRC | CMD_IDX_CHK | CMD_DATA, true,
                   NULL) != 0)
        return -1;

    for (unsigned words = 0; words < 128; words++) {
        unsigned t;

        for (t = 0; t < 1000000; t++)
            if (r32(b + SD_PSTATE) & PSTATE_BUF_RD)
                break;
        if (t == 1000000)
            return -1;
        out[words] = r32(b + SD_BUFFER);
    }

    for (unsigned t = 0; t < 1000000; t++)
        if (r16(b + SD_NINT) & NIS_XFER_DONE) {
            w16(b + SD_NINT, NIS_XFER_DONE);
            return 0;
        }
    return -1;
}

static int sd_write_block_pio(struct sdhci_host *h, uint64_t lba,
                              const void *buf)
{
    uintptr_t b = h->base;
    uint64_t addr = h->sdhc_block_addr ? lba : lba * 512u;
    const uint32_t *in = buf;

    w16(b + SD_BLK_SIZE, 512);
    w16(b + SD_BLK_COUNT, 1);
    w16(b + SD_XFER_MODE, TM_BCEN);     /* write: direction bit clear */

    if (sd_cmd_raw(h, CMD24_WRITE, (uint32_t)addr,
                   RESP_R48 | CMD_CRC | CMD_IDX_CHK | CMD_DATA, false,
                   NULL) != 0)
        return -1;

    for (unsigned words = 0; words < 128; words++) {
        for (unsigned t = 0; t < 1000000; t++)
            if (r32(b + SD_PSTATE) & PSTATE_BUF_WR)
                break;
        w32(b + SD_BUFFER, in[words]);
    }

    /* wait for transfer completion, then for the card to finish programming */
    for (unsigned t = 0; t < 1000000; t++)
        if (r16(b + SD_NINT) & NIS_XFER_DONE) {
            w16(b + SD_NINT, NIS_XFER_DONE);
            goto wait_program;
        }
    return -1;

wait_program:
    for (unsigned guard = 0; guard < 1000; guard++) {
        uint32_t r1 = 0;
        unsigned state;

        if (sd_cmd_r1(h, CMD13_STATUS, h->rca << 16, &r1) != 0)
            continue;
        state = (r1 >> 9) & 0xfu;
        if (state == 4 || state == 3)   /* tran / stby: done writing */
            return 0;
    }
    return -1;
}

/* ---- card identification ---------------------------------------------------------------- */

static int sd_card_init(struct sdhci_host *h)
{
    uint32_t resp[4];
    uint8_t csd[16];
    int v2 = 0;

    /* 400 kHz during identification */
    sd_set_clock(h, 400);

    for (int i = 0; i < 4; i++)
        sd_cmd_raw(h, CMD0_GO_IDLE, 0, RESP_NONE, false, NULL);

    /* voltage check: presence of a valid CMD8 reply means SDv2 */
    if (sd_cmd_raw(h, CMD8_IF_COND, 0x000001aau,
                   RESP_R48 | CMD_CRC, false, resp) == 0 &&
        (resp[0] & 0xffu) == 0xaau)
        v2 = 1;

    /* ACMD41 until the card powers up (OCR bit31) */
    for (unsigned guard = 0; guard < 2000; guard++) {
        uint32_t ocr_arg = (v2 ? 0x40000000u : 0) | 0x00300000u;

        if (sd_acmd(h, ACMD41_SD_SEND_OP, ocr_arg, RESP_R48, resp) != 0)
            return -1;
        if (resp[0] & 0x80000000u) {
            h->sdhc_block_addr = !!(resp[0] & 0x40000000u);
            goto powered;
        }
    }
    return -1;

powered:
    /* CID */
    if (sd_cmd_raw(h, CMD2_ALL_CID, 0, RESP_R136 | CMD_CRC, false, resp) != 0)
        return -1;

    /* RCA (card-chosen, R6) */
    if (sd_cmd_raw(h, CMD3_REL_ADDR, 0, RESP_R48 | CMD_CRC | CMD_IDX_CHK,
                   false, resp) != 0)
        return -1;
    h->rca = resp[0] >> 16;
    if (!h->rca)
        h->rca = 1;

    /* CSD -> geometry (v2.0 layout assumed for SDHC cards) */
    if (sd_cmd_raw(h, CMD9_SEND_CSD, h->rca << 16, RESP_R136 | CMD_CRC,
                   false, resp) != 0)
        return -1;
    csd[15] = (uint8_t)(resp[3] >> 24);
    csd[14] = (uint8_t)(resp[3] >> 16);
    csd[13] = (uint8_t)(resp[3] >> 8);
    csd[12] = (uint8_t)(resp[3]);
    csd[11] = (uint8_t)(resp[2] >> 24);
    csd[10] = (uint8_t)(resp[2] >> 16);
    csd[9]  = (uint8_t)(resp[2] >> 8);
    csd[8]  = (uint8_t)(resp[2]);
    csd[7]  = (uint8_t)(resp[1] >> 24);
    csd[6]  = (uint8_t)(resp[1] >> 16);
    csd[5]  = (uint8_t)(resp[1] >> 8);
    csd[4]  = (uint8_t)(resp[1]);
    csd[3]  = (uint8_t)(resp[0] >> 24);
    csd[2]  = (uint8_t)(resp[0] >> 16);
    csd[1]  = (uint8_t)(resp[0] >> 8);
    csd[0]  = (uint8_t)(resp[0]);

    uint64_t sectors;
    unsigned csd_ver = (csd[14] >> 6) & 0x3u;

    if (csd_ver == 1) {                 /* CSD v2.0 (SDHC/SDXC) */
        /*
         * C_SIZE is bits [69:48]: byte 8 holds [71:64], so the
         * top two usable bits live in csd[8]&0x3f.
         */
        uint32_t csize = ((uint32_t)(csd[8] & 0x3fu) << 16) |
                         ((uint32_t)csd[7] << 8) | csd[6];

        sectors = ((uint64_t)csize + 1) * 1024;
    } else {                            /* CSD v1.0 byte-addressed */
        unsigned read_bl_len = csd[10] & 0x0fu;         /* [83:80] */
        unsigned csize      = ((csd[9] & 0x03u) << 10) |   /* [73:62] */
                              (csd[8] << 2) | (csd[7] >> 6);
        unsigned csize_mult = ((csd[6] & 0x03u) << 1) |    /* [49:47] */
                              (csd[5] >> 7);

        sectors = ((uint64_t)csize + 1) << (csize_mult + 2);
        sectors >>= read_bl_len >= 9 ? read_bl_len - 9 : 0;
    }
    if (!sectors)
        return -1;

    /* select it, widen the bus, fix block length at 512 */
    if (sd_cmd_raw(h, CMD7_SELECT, h->rca << 16,
                   RESP_R48B | CMD_CRC, false, NULL) != 0)
        return -1;
    sd_acmd(h, 6, 0x2, RESP_R48 | CMD_CRC, NULL);       /* 4-bit bus */
    {
        uintptr_t b = h->base;

        w8(b + SD_HOST_CTRL, (uint8_t)(r8(b + SD_HOST_CTRL) | HOST_4BIT));
    }
    if (sd_cmd_r1(h, CMD16_SET_LEN, 512, NULL) != 0)
        return -1;

    /* operation clock */
    sd_set_clock(h, 25000);

    h->bd.capacity_sectors = sectors;
    return 0;
}

/* ---- block_device glue ------------------------------------------------------------------------ */

static int mmc_read_blocks(struct block_device *bd, uint64_t lba,
                           void *buf, unsigned nsect)
{
    struct sdhci_host *h = bd->priv;
    daif_state s;
    int r = 0;
    uint8_t *p = buf;

    spin_lock_irqsave(&h->lock, &s);
    for (unsigned i = 0; i < nsect && r == 0; i++)
        r = sd_read_block_pio(h, lba + i, p + i * 512);
    spin_unlock_irqrestore(&h->lock, s);
    return r;
}

static int mmc_write_blocks(struct block_device *bd, uint64_t lba,
                            const void *buf, unsigned nsect)
{
    struct sdhci_host *h = bd->priv;
    daif_state s;
    int r = 0;
    const uint8_t *p = buf;

    spin_lock_irqsave(&h->lock, &s);
    for (unsigned i = 0; i < nsect && r == 0; i++)
        r = sd_write_block_pio(h, lba + i, p + i * 512);
    spin_unlock_irqrestore(&h->lock, s);
    return r;
}

int sdhci_register(uintptr_t mmio_base, const char *name)
{
    struct sdhci_host *h;
    uintptr_t b = mmio_base;

    if (s_nhosts >= ARRAY_SIZE(s_hosts))
        return -1;

    h = kzalloc(sizeof(*h));
    if (!h)
        return -1;
    h->base = mmio_base;
    h->lock = (spinlock_t)SPINLOCK_INIT;
    h->name[0] = '\0';
    for (unsigned i = 0; name && name[i]; i++)
        if (i < sizeof(h->name))
            h->name[i] = name[i];

    /* controller reset + power + conservative timeouts */
    sd_reset(h, RST_ALL);
    w8(b + SD_POWER, 0xf);              /* 3.3V + bus power */
    w8(b + SD_TIMEOUT, 0xe);
    w16(b + SD_NINT_EN, 0xffff);
    w16(b + SD_EINT_EN, 0xffff);
    (void)r16(b + SD_NINT);             /* clear pending */
    (void)r16(b + SD_EINT);

    if (sd_card_init(h) != 0) {
        kprintf("mmc: %s no card found\n", h->name);
        kfree(h);
        return -1;
    }

    h->bd.name         = h->name;
    h->bd.priv         = h;
    h->bd.max_sectors  = 1;             /* single-block protocol */
    h->bd.read_blocks  = mmc_read_blocks;
    h->bd.write_blocks = mmc_write_blocks;

    if (block_register(&h->bd) != 0) {
        kfree(h);
        return -1;
    }

    s_hosts[s_nhosts++] = h;
    return 0;
}
