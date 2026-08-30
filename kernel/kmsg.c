/*
 * kmsg.c - the kernel message ring (phase 16, plan item 87).
 *
 * The single writer is printf.c's emit path: it already holds the
 * UART tx lock (or runs in raw panic mode, where the machine is
 * single-threaded by construction), so the ring needs no lock of
 * its own. Readers tolerate a torn line in the worst case -- the
 * content is diagnostic, never control data.
 *
 * kmsg_dump() persists the oldest-first ring to the VFS; the panic
 * path calls it before halting, which is the "kmsg ring persisted
 * to flash" item on boards with a writable root.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kmsg.h"
#include "lib.h"
#include "syscall.h"
#include "vfs.h"

/*
 * Sized to hold a whole verbose boot, selftests included (~25 KiB on
 * QEMU). At 8 KiB the ring wrapped before the release battery ran, so
 * the banner and the bring-up lines -- the part worth reading after a
 * reset -- were the first thing lost.
 */
#define KMSG_BYTES  65536u

static char     kmsg_buf[KMSG_BYTES];
static unsigned kmsg_head;              /* next write slot           */
static uint64_t kmsg_total;             /* bytes ever written        */

void kmsg_putc(char c)
{
    kmsg_buf[kmsg_head] = c;
    kmsg_head = (kmsg_head + 1u) % KMSG_BYTES;
    kmsg_total++;
}

unsigned kmsg_count(void)
{
    unsigned fill = kmsg_total > KMSG_BYTES ? KMSG_BYTES
                                            : (unsigned)kmsg_total;
    unsigned lines = 0;

    /* count complete (newline-terminated) lines, oldest first     */
    for (unsigned i = 0; i < fill; i++) {
        char c = kmsg_buf[(kmsg_head + KMSG_BYTES - fill + i) %
                          KMSG_BYTES];

        if (c == '\n')
            lines++;
    }
    return lines;
}

bool kmsg_line(unsigned idx, char *out, unsigned cap)
{
    unsigned fill = kmsg_total > KMSG_BYTES ? KMSG_BYTES
                                            : (unsigned)kmsg_total;
    unsigned start = (kmsg_head + KMSG_BYTES - fill) % KMSG_BYTES;
    unsigned i = 0, line = 0;
    unsigned len = 0;
    bool copying = false;

    if (!out || !cap)
        return false;

    while (i < fill) {
        char c = kmsg_buf[(start + i) % KMSG_BYTES];

        i++;
        if (!copying) {
            if (c == '\n')
                continue;           /* skip empties               */
            copying = true;
            len = 0;
        }
        if (c == '\n') {
            if (line == idx) {
                out[len < cap ? len : cap - 1] = 0;
                return true;
            }
            line++;
            copying = false;
            continue;
        }
        if (line == idx && len + 1 < cap)
            out[len++] = c;
    }

    if (copying && line == idx) {   /* unterminated tail line     */
        out[len < cap ? len : cap - 1] = 0;
        return true;
    }
    return false;
}

int kmsg_dump(const char *path)
{
    unsigned fill = kmsg_total > KMSG_BYTES ? KMSG_BYTES
                                            : (unsigned)kmsg_total;
    unsigned start = (kmsg_head + KMSG_BYTES - fill) % KMSG_BYTES;
    struct file *f;
    int r = 0;

    if (!path)
        return -EINVAL;

    if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f))
        return -EIO;                /* root not mounted / ro      */

    {
        /* the live window is contiguous in logical order but may
         * wrap physically: emit [start, start+first) then [0, ..) */
        unsigned first = KMSG_BYTES - start;

        if (first > fill)
            first = fill;

        if (first && f_write(f, &kmsg_buf[start], first) < 0)
            r = -EIO;
        if (!r && fill > first &&
            f_write(f, &kmsg_buf[0], fill - first) < 0)
            r = -EIO;
    }
    file_close(f);
    return r;
}
