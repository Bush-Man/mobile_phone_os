/*
 * i2c_core.c - adapter registry, transfer helpers, and generic
 * device probing from FDT children of an I2C controller node.
 */

#include <stdint.h>
#include <stddef.h>

#include "fdt.h"
#include "i2c.h"
#include "lib.h"

static struct i2c_adapter *adapters[I2C_ADAPT_MAX];
static unsigned nadapters;

struct i2c_client clients[I2C_CLIENT_MAX];
unsigned nclients;

static bool s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned long s_len(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    return n;
}

int i2c_adapter_register(struct i2c_adapter *ad)
{
    if (!ad || !ad->name || !ad->xfer)
        return -1;
    for (unsigned i = 0; i < nadapters; i++)
        if (adapters[i] == ad)
            return 0;                   /* idempotent */
    if (nadapters >= I2C_ADAPT_MAX)
        return -1;
    adapters[nadapters++] = ad;
    kprintf("i2c: adapter %s @ %u kHz\n", ad->name,
            ad->speed_khz ? ad->speed_khz : 100);
    return 0;
}

struct i2c_adapter *i2c_adapter_find(const char *name)
{
    if (!name)
        return NULL;
    for (unsigned i = 0; i < nadapters; i++)
        if (s_eq(adapters[i]->name, name))
            return adapters[i];
    return NULL;
}

unsigned i2c_adapter_count(void)
{
    return nadapters;
}

int i2c_transfer(struct i2c_adapter *ad, struct i2c_msg *msgs,
                 unsigned nmsgs)
{
    if (!ad || !msgs || nmsgs == 0)
        return -1;
    return ad->xfer(ad, msgs, nmsgs);
}

int i2c_write_bytes(struct i2c_adapter *ad, uint8_t addr,
                    const uint8_t *src, uint16_t len)
{
    struct i2c_msg m = {
        .addr = addr,
        .flags = 0,
        .len = len,
        .buf = (uint8_t *)src,
    };

    return i2c_transfer(ad, &m, 1);
}

int i2c_read_bytes(struct i2c_adapter *ad, uint8_t addr,
                   uint8_t *dst, uint16_t len)
{
    struct i2c_msg m = {
        .addr = addr,
        .flags = I2C_M_RD,
        .len = len,
        .buf = dst,
    };

    return i2c_transfer(ad, &m, 1);
}

/* register write: [reg][val] in one transaction */
int i2c_write_reg8(struct i2c_adapter *ad, uint8_t addr,
                   uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    struct i2c_msg m = { .addr = addr, .len = 2, .buf = b };

    return i2c_transfer(ad, &m, 1);
}

/* register read: write pointer with repeated start, then read */
int i2c_read_reg8(struct i2c_adapter *ad, uint8_t addr,
                  uint8_t reg, uint8_t *val)
{
    struct i2c_msg m[2] = {
        { .addr = addr, .len = 1, .buf = &reg },
        { .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = val },
    };

    return i2c_transfer(ad, m, 2);
}

int i2c_probe_addr(struct i2c_adapter *ad, uint8_t addr)
{
    if (addr <= 0x07 || addr >= 0x78)   /* reserved addresses */
        return -1;

    /* quick probe: address phase only, zero data bytes */
    struct i2c_msg m = { .addr = addr, .len = 0, .buf = NULL };

    return i2c_transfer(ad, &m, 1);
}

/* ---- FDT child discovery -------------------------------------------------------------- */

static const uint32_t *skip_subtree(const uint32_t *cur)
{
    /* cur sits just past a node's name field */
    int depth = 1;

    while (depth > 0) {
        switch (fdt_u32(cur)) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)(cur + 1);

            cur += 1 + (s_len(name) + 4) / 4;
            depth++;
            break;
        }
        case FDT_END_NODE:
            cur++;
            depth--;
            break;
        case FDT_PROP:
            cur += 3 + (fdt_u32(cur + 1) + 3) / 4;
            break;
        default:
            cur++;
            depth--;                    /* bail out on garbage/FDT_END */
            break;
        }
    }
    return cur;
}

static void record_client(struct i2c_adapter *ad, uint32_t addr,
                          const char *compat, unsigned *found)
{
    if (*found >= I2C_CLIENT_MAX || nclients >= I2C_CLIENT_MAX)
        return;
    if (addr > 0x7f)
        return;

    struct i2c_client *cl = &clients[nclients++];

    cl->adap      = ad;
    cl->addr      = (uint8_t)addr;
    cl->compat    = compat;
    cl->probed_ok = 0;
    (*found)++;
}

/*
 * Scans one level of direct children of the controller node.
 * 'cur' points at the first token of the controller's contents.
 */
static const uint32_t *scan_ctrl_children(struct fdt *f,
                                          const uint32_t *cur,
                                          struct i2c_adapter *ad,
                                          unsigned *found)
{
    while (1) {
        switch (fdt_u32(cur)) {
        case FDT_NOP:
            cur++;
            break;

        case FDT_PROP:
            cur += 3 + (fdt_u32(cur + 1) + 3) / 4;
            break;

        case FDT_END_NODE:
            return cur + 1;

        case FDT_BEGIN_NODE: {
            const char *name = (const char *)(cur + 1);
            const uint32_t *pc =
                cur + 1 + (s_len(name) + 4) / 4;
            uint32_t addr = 0xffffffffu;
            const char *compat = NULL;

            /* read this child's own property stream */
            while (fdt_u32(pc) == FDT_PROP) {
                uint32_t plen = fdt_u32(pc + 1);
                uint32_t noff = fdt_u32(pc + 2);
                const char *pname = f->strings + noff;
                const void *pv = pc + 3;

                if (plen >= 4 && pname[0] == 'r' &&
                    pname[1] == 'e' && pname[2] == 'g' && !pname[3])
                    addr = fdt_u32(pv);

                if (!compat && pname[0] == 'c' && pname[1] == 'o' &&
                    pname[2] == 'm')
                    compat = pv;

                pc += 3 + (plen + 3) / 4;
            }

            if (addr != 0xffffffffu)
                record_client(ad, addr, compat, found);

            cur = skip_subtree(cur);
            break;
        }

        default:
            return NULL;                /* FDT_END or malformed */
        }
    }
}

int i2c_enumerate_fdt_children(struct i2c_adapter *ad,
                               const void *fdt_blob, int ctrl_node)
{
    struct fdt f;
    const uint32_t *cur;
    const char *node_name;
    unsigned found = 0;

    if (!ad || !fdt_blob || ctrl_node < 0)
        return -1;
    if (fdt_init(&f, (uintptr_t)fdt_blob) != 0)
        return -1;

    cur = (const uint32_t *)((const uint8_t *)f.hdr + ctrl_node);
    if (fdt_u32(cur) != FDT_BEGIN_NODE)
        return -1;

    node_name = (const char *)(cur + 1);
    cur = cur + 1 + (s_len(node_name) + 4) / 4;

    cur = scan_ctrl_children(&f, cur, ad, &found);
    return cur ? (int)found : -1;
}
