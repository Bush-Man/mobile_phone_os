/*
 * sms.c - GSM 03.38 7-bit septet packing, SMS-SUBMIT/DELIVER PDU
 * build/parse, and the filesystem-backed message store (phase 12,
 * item 67).
 *
 * Packing uses a straight bit-accumulator (septet i lands at bit
 * offset i*7 in the output stream); the alphabet table covers the
 * printable ASCII range plus the classic remapped specials, and
 * unknown characters fall back to space so round-trips never run
 * away. Messages are stored under /sms/msg<N> as "FROM <sender>\n
 * <text>" through the phase-7 VFS (ramfs on QEMU, vfat/ext2 on
 * boards once mounted).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "lib.h"
#include "modem.h"
#include "vfs.h"

/* ---- 7-bit alphabet ---------------------------------------------------------------- */

static uint8_t char_to_gsm(char c)
{
    switch (c) {
    case '@': return 0x00u;
    case '$': return 0x02u;
    case '\n': return 0x0au;
    case '\r': return 0x0du;
    default:
        if (c >= ' ' && c <= '_')
            return (uint8_t)(c - ' ' + 0x20u);
        if (c >= 'a' && c <= 'z')
            return (uint8_t)(c - 'a' + 0x61u);
        return 0x20u;                   /* space fallback             */
    }
}

static char gsm_to_char(uint8_t g)
{
    switch (g) {
    case 0x00u: return '@';
    case 0x02u: return '$';
    case 0x0au: return '\n';
    case 0x0du: return '\r';
    default:
        if (g >= 0x20u && g <= 0x5fu)
            return (char)(' ' + g - 0x20u);
        if (g >= 0x61u && g <= 0x7au)
            return (char)('a' + g - 0x61u);
        return ' ';
    }
}

/* ---- septet packing ---------------------------------------------------------------- */

int sms_encode_7bit(const char *text, uint8_t *out, unsigned cap)
{
    unsigned bitpos = 0;
    unsigned bytes;

    if (!out || !cap)
        return -1;
    memset(out, 0, cap);

    for (const char *p = text; *p; p++) {
        uint8_t g = char_to_gsm(*p);
        unsigned byte = bitpos >> 3;
        unsigned shift = bitpos & 7u;

        if (byte >= cap)
            return -1;
        out[byte] |= (uint8_t)(g << shift);
        if (shift && byte + 1u < cap)
            out[byte + 1u] |= (uint8_t)(g >> (8u - shift));
        bitpos += 7u;
    }

    bytes = (bitpos + 7u) / 8u;
    return (int)bytes;                  /* callers convert to septets*/
}

int sms_decode_7bit(const uint8_t *in, unsigned septets,
                    char *out, unsigned cap)
{
    unsigned bitpos = 0;
    unsigned n = 0;

    for (unsigned i = 0; i < septets; i++) {
        unsigned byte = bitpos >> 3;
        unsigned shift = bitpos & 7u;
        uint8_t g;

        if (n + 1u >= cap)
            break;
        g = (uint8_t)(in[byte] >> shift);
        if (shift)
            g |= (uint8_t)(in[byte + 1u] << (8u - shift));
        g &= 0x7fu;

        out[n++] = gsm_to_char(g);
        bitpos += 7u;
    }
    out[n] = 0;
    return (int)n;
}

/* ---- addresses ------------------------------------------------------------------------ */

/* digits -> BCD nibble swap, 'F' padded; returns byte count       */
static int addr_bcd(const char *digits, uint8_t *out, unsigned cap)
{
    unsigned n = (unsigned)strlen(digits);
    unsigned o = 0;

    for (unsigned i = 0; i < n; i += 2) {
        uint8_t lo = (uint8_t)(digits[i] - '0');
        uint8_t hi = (i + 1u < n) ? (uint8_t)(digits[i + 1u] - '0')
                                  : 0x0fu;

        if (o >= cap)
            return -1;
        if (lo > 9u) lo = 0u;
        if (hi > 9u) hi = 0x0fu;
        out[o++] = (uint8_t)(lo | (hi << 4));
    }
    return (int)o;
}

static void addr_bcd_decode(const uint8_t *in, unsigned ndigits,
                            char *out, unsigned cap)
{
    unsigned n = 0;

    for (unsigned i = 0; i < ndigits && n + 1u < cap; i++) {
        uint8_t b = in[i / 2u];
        uint8_t nib = (i & 1u) ? (b >> 4) : (b & 0x0fu);

        if (nib > 9u)
            break;                      /* F pad                      */
        out[n++] = (char)('0' + nib);
    }
    out[n] = 0;
}

/* ---- PDUs ------------------------------------------------------------------------------ */

int sms_build_submit_pdu(const char *to, const char *text,
                         uint8_t *pdu, unsigned cap)
{
    uint8_t ud[160];
    int ud_bytes, septets;
    unsigned o = 0;
    unsigned digits = (unsigned)strlen(to);
    int ab;

    if (!pdu || cap < 32u)
        return -1;

    septets = (int)strlen(text);
    ud_bytes = sms_encode_7bit(text, ud, sizeof(ud));
    if (ud_bytes < 0)
        return -1;

    pdu[o++] = 0x00u;                   /* SMSC: none                 */
    pdu[o++] = 0x01u;                   /* SMS-SUBMIT, VPF=0          */
    pdu[o++] = 0x00u;                   /* MR                         */
    pdu[o++] = (uint8_t)digits;
    pdu[o++] = 0x81u;                   /* TON unknown / NPI ISDN     */
    ab = addr_bcd(to, &pdu[o], cap - o);
    if (ab < 0)
        return -1;
    o += (unsigned)ab;
    pdu[o++] = 0x00u;                   /* PID                        */
    pdu[o++] = 0x00u;                   /* DCS: 7-bit default         */
    pdu[o++] = (uint8_t)septets;        /* UDL (septets)              */
    if (o + (unsigned)ud_bytes > cap)
        return -1;
    memcpy(&pdu[o], ud, (size_t)ud_bytes);
    o += (unsigned)ud_bytes;
    return (int)o;
}

int sms_parse_deliver_pdu(const uint8_t *pdu, unsigned len,
                          char *sender, unsigned sender_cap,
                          char *text, unsigned text_cap)
{
    unsigned o = 0;
    unsigned oa_digits;
    unsigned udl, septets;

    if (len < 2u)
        return -1;

    o = 1;                              /* flags octet                */
    if (o >= len)
        return -1;
    oa_digits = pdu[o++];
    o++;                                /* OA type octet              */
    if (o + (oa_digits + 1u) / 2u + 2u + 7u + 1u > len)
        return -1;

    addr_bcd_decode(&pdu[o], oa_digits, sender, sender_cap);
    o += (oa_digits + 1u) / 2u;
    o += 2u;                            /* PID + DCS                  */
    o += 7u;                            /* SCTS                       */

    udl     = pdu[o++];
    septets = udl * 7u / 8u;
    if (septets == 0u)
        septets = udl;
    if (o + (udl * 7u + 7u) / 8u > len)
        return -1;

    sms_decode_7bit(&pdu[o], septets, text, text_cap);
    return 0;
}

/* ---- message store (filesystem-backed) ----------------------------------------------- */

static unsigned msg_seq;

unsigned sms_seq(void)
{
    return msg_seq;
}

int sms_store_inbox(const char *sender, const char *text)
{
    char path[24], line[192];
    struct file *f;
    int fd, r;

    vfs_mkdir("/sms");                  /* EEXIST is fine             */

    msg_seq++;
    /* simple decimal name                                         */
    {
        char num[8];
        unsigned v = msg_seq, i = 0;

        do {
            num[i++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        memcpy(path, "/sms/msg", 8);
        {
            unsigned o = 8;

            while (i)
                path[o++] = num[--i];
            path[o] = 0;
        }
    }

    {
        int sl = (int)strlen(sender);

        memcpy(line, "FROM ", 5);
        memcpy(&line[5], sender, (size_t)sl);
        line[5u + sl] = '\n';
        memcpy(&line[6u + sl], text, strlen(text));
        line[6u + sl + strlen(text)] = 0;
    }

    r = vfs_open(path, O_CREAT | O_WRONLY, &f);
    if (r)
        return -1;
    fd = 0;
    (void)fd;
    r = f_write(f, line, strlen(line));
    file_close(f);
    return r > 0 ? 0 : -1;
}

int sms_read_msg(const char *name, char *sender,
                 unsigned sender_cap, char *text, unsigned text_cap)
{
    struct file *f;
    char buf[192];
    int r;
    const char *nl;
    size_t textlen;

    r = vfs_open(name, O_RDONLY, &f);
    if (r)
        return -1;
    r = f_read(f, buf, sizeof(buf) - 1u);
    file_close(f);
    if (r <= 0)
        return -1;
    buf[r] = 0;

    if (strncmp(buf, "FROM ", 5))
        return -1;
    nl = strchr(&buf[5], '\n');
    if (!nl)
        return -1;

    {
        size_t sl = (size_t)(nl - &buf[5]);

        if (sl >= sender_cap)
            sl = sender_cap - 1u;
        memcpy(sender, &buf[5], sl);
        sender[sl] = 0;
    }
    textlen = strlen(nl + 1);
    if (textlen >= text_cap)
        textlen = text_cap - 1u;
    memcpy(text, nl + 1, textlen);
    text[textlen] = 0;
    return 0;
}
