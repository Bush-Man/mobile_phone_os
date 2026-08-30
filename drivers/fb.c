/*
 * fb.c - framebuffer core: canvases of scattered 4K pages, blitters
 * and the built-in 8x8 font (phase 9).
 *
 * All CPU touches go through the kernel's uncached device-window
 * alias per page (vmm_dmap), matching the virtio stack convention:
 * host-side GPU/device consumers see our writes coherently without a
 * single cache-maintenance instruction anywhere.
 *
 * The span iterator is the heart of every primitive: logical byte
 * offsets into the linear framebuffer decompose into (cpu ptr,len)
 * runs bounded by page frames, since backends may hand out disjoint
 * physical pages for their scatter-gather resource backing.
 *
 * Backends must keep stride_bytes <= 4096 so a full scanline always
 * snapshots into one page-sized scratch buffer for overlap-safe
 * copies; fb_claim_default rejects anything wider up front.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "lib.h"
#include "mm/types.h"
#include "mm/vmm.h"
#include "panic.h"
#include "spinlock.h"
#include "fb.h"

#define FB_MAX_STRIDE   4096u
#define FB_BACKEND_MAX  4

/* ---- backend registry ------------------------------------------------------------ */

static const struct fb_backend *backends[FB_BACKEND_MAX];
static unsigned nbackends;

static spinlock_t reg_lock = SPINLOCK_INIT;

static struct {
    bool        active;
    const struct fb_canvas *cv;
} disp;

int fb_backend_register(const struct fb_backend *b)
{
    daif_state s;
    unsigned pos;

    if (!b || !b->name || !b->claim)
        return -1;

    spin_lock_irqsave(&reg_lock, &s);
    if (nbackends >= FB_BACKEND_MAX) {
        spin_unlock_irqrestore(&reg_lock, s);
        kprintf("fb: backend table full (%s)\n", b->name);
        return -1;
    }

    /* stable insertion by descending priority                      */
    pos = nbackends;
    while (pos > 0 && backends[pos - 1]->priority < b->priority) {
        backends[pos] = backends[pos - 1];
        pos--;
    }
    backends[pos] = b;
    nbackends++;
    spin_unlock_irqrestore(&reg_lock, s);
    return 0;
}

int fb_claim_default(struct fb_canvas *out)
{
    static struct fb_canvas held;
    const struct fb_backend *cand[FB_BACKEND_MAX];
    unsigned n;
    daif_state s;

    if (!out)
        return -1;

    /*
     * claim() arms real hardware: the virtio-gpu backend blocks on a
     * command round-trip whose completion arrives by IRQ. So snapshot
     * the registry under the lock and run the claims with IRQs live
     * and no lock held -- calling claim() from inside the critical
     * section deadlocks the caller against its own completion IRQ.
     */
    spin_lock_irqsave(&reg_lock, &s);
    n = nbackends;
    for (unsigned i = 0; i < n; i++)
        cand[i] = backends[i];
    spin_unlock_irqrestore(&reg_lock, s);

    for (unsigned i = 0; i < n; i++) {
        struct fb_canvas cv;

        memset(&cv, 0, sizeof(cv));
        if (cand[i]->claim(&cv) != 0)
            continue;

        if (cv.stride_bytes == 0 ||
            cv.width == 0 || cv.height == 0 ||
            !cv.frames || cv.nframes == 0 ||
            cv.stride_bytes > FB_MAX_STRIDE) {
            kprintf("fb: backend %s returned bad geometry\n",
                    cand[i]->name);
            continue;
        }

        spin_lock_irqsave(&reg_lock, &s);
        held = cv;
        disp.cv = &held;
        disp.active = true;
        spin_unlock_irqrestore(&reg_lock, s);

        memcpy(out, &cv, sizeof(*out));
        kprintf("fb: canvas \"%s\" %ux%u from %s (%s)\n",
                out->name, out->width, out->height, cand[i]->name,
                out->double_buffered ? "double" : "single");
        return 0;
    }
    return -1;
}

bool fb_present(void)
{
    daif_state s;
    bool r;

    spin_lock_irqsave(&reg_lock, &s);
    r = disp.active;
    spin_unlock_irqrestore(&reg_lock, s);
    return r;
}

const struct fb_canvas *fb_active(void)
{
    daif_state s;
    const struct fb_canvas *r;

    spin_lock_irqsave(&reg_lock, &s);
    r = disp.active ? disp.cv : NULL;
    spin_unlock_irqrestore(&reg_lock, s);
    return r;
}

/* ---- span iteration ------------------------------------------------------------------ */

/*
 * Byte offset `off` inside a canvas decomposes into page index and
 * in-page offset; each run is bounded by the frame's end.
 */
static inline unsigned cv_page_of(uint64_t off)
{
    return (unsigned)(off >> PAGE_SHIFT);
}

static uint8_t *cv_cpu(const struct fb_canvas *cv, uint64_t off)
{
    /* off must be validated by callers (region clamp first)        */
    paddr_t pa = cv->frames[cv_page_of(off)] +
                 (off & (PAGE_SIZE - 1));

    return (uint8_t *)(uintptr_t)vmm_dmap(pa);
}

/*
 * Invoke fn(cpulen-ptr,len) for every contiguous run covering
 * [off, off+len). Used with a tiny closure instead of callbacks? --
 * plain function-pointer form keeps the three blitters honest.
 */
typedef void (*span_fn)(uint8_t *va, size_t n, void *arg);

static void span_walk(const struct fb_canvas *cv, uint64_t off,
                      size_t len, span_fn fn, void *arg)
{
    while (len) {
        uint64_t in_page = PAGE_SIZE - (off & (PAGE_SIZE - 1));
        size_t   chunk  = (size_t)in_page;

        if ((uint64_t)chunk > len)
            chunk = len;
        fn(cv_cpu(cv, off), chunk, arg);
        off += chunk;
        len -= chunk;
    }
}

/* ---- pixel primitives --------------------------------------------------------------- */

struct fill_arg {
    uint32_t word;                      /* XRGB8888                   */
    unsigned rem;                       /* pixels left in this row    */
};

static void fill_span(uint8_t *va, size_t n, void *raw)
{
    struct fill_arg *a = raw;
    size_t px = n / FB_BPP;

    if ((size_t)a->rem < px)
        px = a->rem;
    for (size_t i = 0; i < px; i++)
        memcpy(&va[i * FB_BPP], &a->word, FB_BPP);
    a->rem -= (unsigned)px;
}

void fb_fill_rect(struct fb_canvas *cv, unsigned x, unsigned y,
                  unsigned w, unsigned h, uint32_t c)
{
    if (!cv || !cv->frames || !cv->width)
        return;

    /* clamp                                                        */
    if (x >= cv->width || y >= cv->height)
        return;
    if (w > cv->width - x)
        w = cv->width - x;
    if (h > cv->height - y)
        h = cv->height - y;

    for (unsigned row = 0; row < h; row++) {
        uint64_t off = (uint64_t)(y + row) * cv->stride_bytes +
                       (uint64_t)x * FB_BPP;
        struct fill_arg arg = { .word = c, .rem = w };

        span_walk(cv, off, (size_t)w * FB_BPP, fill_span, &arg);
    }
}

void fb_put_pixel(struct fb_canvas *cv, unsigned x, unsigned y,
                  uint32_t c)
{
    fb_fill_rect(cv, x, y, 1, 1, c);
}

/* overlap-safe line snapshot: read the WHOLE source line first     */
static void copy_line(struct fb_canvas *dst, const struct fb_canvas *src,
                      unsigned dx, unsigned dy,
                      unsigned sx, unsigned sy, unsigned w)
{
    uint64_t soff = (uint64_t)sy * src->stride_bytes +
                    (uint64_t)sx * FB_BPP;
    uint64_t doff = (uint64_t)dy * dst->stride_bytes +
                    (uint64_t)dx * FB_BPP;
    size_t   bytes = (size_t)w * FB_BPP;
    static uint8_t scratch[FB_MAX_STRIDE];      /* one line          */

    /* source may straddle pages: gather into the line buffer       */
    {
        size_t done = 0;

        while (done < bytes) {
            uint64_t in_page = PAGE_SIZE -
                               ((soff + done) & (PAGE_SIZE - 1));
            size_t chunk = (size_t)in_page;

            if ((uint64_t)chunk > bytes - done)
                chunk = bytes - done;
            memcpy(&scratch[done],
                   cv_cpu(src, soff + done), chunk);
            done += chunk;
        }
    }

    /* scatter the staged line, page-bounded again                  */
    {
        size_t done = 0;

        while (done < bytes) {
            uint64_t in_page = PAGE_SIZE -
                               ((doff + done) & (PAGE_SIZE - 1));
            size_t chunk = (size_t)in_page;

            if ((uint64_t)chunk > bytes - done)
                chunk = bytes - done;
            memcpy(cv_cpu(dst, doff + done),
                   &scratch[done], chunk);
            done += chunk;
        }
    }
}

static void clamp_box(unsigned cvw, unsigned cvh,
                      unsigned *x, unsigned *y,
                      unsigned *w, unsigned *h)
{
    if (*x >= cvw || *y >= cvh) {
        *w = 0;
        *h = 0;
        return;
    }
    if (*w > cvw - *x)
        *w = cvw - *x;
    if (*h > cvh - *y)
        *h = cvh - *y;
}

void fb_copy_rect(struct fb_canvas *dst, unsigned dx, unsigned dy,
                  const struct fb_canvas *src, unsigned sx, unsigned sy,
                  unsigned w, unsigned h)
{
    if (!dst || !src || !dst->frames || !src->frames)
        return;

    clamp_box(dst->width, dst->height, &dx, &dy, &w, &h);
    clamp_box(src->width, src->height, &sx, &sy, &w, &h);

    for (unsigned r = 0; r < h; r++)
        copy_line(dst, src, dx, dy + r, sx, sy + r, w);
}

/* ---- alpha blending ------------------------------------------------------------------ */

/*
 * Blend needs both sides of a pixel simultaneously, which the
 * one-direction span walker cannot express cleanly: do it per line
 * with the same gather/scatter pattern as copy_line and integer
 * coverage math ((s*a + d*(255-a) + 127) / 255).
 */
void fb_blend_rect(struct fb_canvas *dst, unsigned dx, unsigned dy,
                   const struct fb_canvas *src, unsigned sx, unsigned sy,
                   unsigned w, unsigned h, uint8_t a)
{
    if (!dst || !src || !dst->frames || !src->frames)
        return;
    if (a == 0)
        return;

    clamp_box(dst->width, dst->height, &dx, &dy, &w, &h);
    clamp_box(src->width, src->height, &sx, &sy, &w, &h);

    for (unsigned r = 0; r < h; r++) {
        uint64_t soff = (uint64_t)(sy + r) * src->stride_bytes +
                        (uint64_t)sx * FB_BPP;
        uint64_t doff = (uint64_t)(dy + r) * dst->stride_bytes +
                        (uint64_t)dx * FB_BPP;

        for (unsigned px = 0; px < w; px++) {
            uint32_t s, d;
            unsigned ch[3];

            memcpy(&s, cv_cpu(src, soff + px * FB_BPP), FB_BPP);
            memcpy(&d, cv_cpu(dst, doff + px * FB_BPP), FB_BPP);

            for (int c = 0; c < 3; c++) {
                unsigned sv = (s >> (16 - 8 * c)) & 0xffu;
                unsigned dv = (d >> (16 - 8 * c)) & 0xffu;

                ch[c] = (sv * a + dv * (255u - a) + 127u) / 255u;
            }
            s = 0xff000000u | (ch[0] << 16) |
                (ch[1] << 8) | ch[2];
            memcpy(cv_cpu(dst, doff + px * FB_BPP), &s, FB_BPP);
        }
    }
}

/* ---- hashing ---------------------------------------------------------------------------- */

uint32_t fb_region_hash(const struct fb_canvas *cv, unsigned x,
                        unsigned y, unsigned w, unsigned h)
{
    uint32_t hash = 0x811c9dc5u;        /* FNV offset basis           */

    if (!cv || !cv->frames)
        return hash;

    clamp_box(cv->width, cv->height, &x, &y, &w, &h);

    for (unsigned r = 0; r < h; r++) {
        uint64_t off = (uint64_t)(y + r) * cv->stride_bytes +
                       (uint64_t)x * FB_BPP;
        size_t   bytes = (size_t)w * FB_BPP;

        while (bytes) {
            uint64_t in_page = PAGE_SIZE -
                               (off & (PAGE_SIZE - 1));
            size_t chunk = (size_t)in_page;

            if ((uint64_t)chunk > bytes)
                chunk = bytes;
            for (size_t i = 0; i < chunk; i++) {
                hash ^= cv_cpu(cv, off)[i];
                hash *= 0x01000193u;
            }
            off += chunk;
            bytes -= chunk;
        }
    }
    return hash ? hash : 1u;            /* never report a zero sum    */
}

/* ---- 8x8 bitmap font --------------------------------------------------------------- */

/*
 * Rows are stored top-down, MSB = leftmost pixel. The set covers
 * everything the on-screen test banners print; unknown characters
 * fall back to a filled box so callers never disappear silently.
 */
struct glyph8x8 {
    uint8_t row[FB_FONT_H];
};

static const struct glyph8x8 font_unknown = {
    { 0xff, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xff },
};

static const struct glyph8x8 font_glyphs[] = {
    [' '] = { { 0 } },                      /* blank               */
    ['-'] = { { 0, 0, 0, 0, 0x7e, 0, 0, 0 } },
    ['.'] = { { 0, 0, 0, 0, 0, 0, 0x18, 0x18 } },
    [':'] = { { 0, 0x18, 0x18, 0, 0, 0x18, 0x18, 0 } },
    ['/'] = { { 0x03, 0x03, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0 } },
    ['#'] = { { 0x24, 0x24, 0xff, 0x24, 0x24, 0xff, 0x24, 0x24 } },
    ['='] = { { 0, 0, 0x7e, 0, 0x7e, 0, 0, 0 } },
    ['+'] = { { 0, 0x18, 0x18, 0x7e, 0x18, 0x18, 0, 0 } },

    ['0'] = { { 0x7e, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x7e } },
    ['1'] = { { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e } },
    ['2'] = { { 0x7e, 0xc3, 0x03, 0x07, 0x1e, 0x78, 0xe0, 0xff } },
    ['3'] = { { 0x7e, 0xc3, 0x03, 0x03, 0x3e, 0x03, 0xc3, 0x7e } },
    ['4'] = { { 0x0c, 0x1c, 0x3c, 0x6c, 0xcc, 0xff, 0x0c, 0x0c } },
    ['5'] = { { 0xff, 0xc0, 0xfe, 0x03, 0x03, 0x03, 0xc3, 0x7e } },
    ['6'] = { { 0x3e, 0x60, 0xc0, 0xfe, 0xc3, 0xc3, 0xc3, 0x7e } },
    ['7'] = { { 0xff, 0x03, 0x06, 0x0c, 0x18, 0x30, 0x30, 0x30 } },
    ['8'] = { { 0x7e, 0xc3, 0xc3, 0x7e, 0xc3, 0xc3, 0xc3, 0x7e } },
    ['9'] = { { 0x7e, 0xc3, 0xc3, 0xc3, 0x7f, 0x03, 0x06, 0x7c } },

    ['A'] = { { 0x18, 0x3c, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x66 } },
    ['B'] = { { 0xfc, 0x66, 0x66, 0x7c, 0x66, 0x66, 0x66, 0xfc } },
    ['C'] = { { 0x3c, 0x66, 0xc0, 0xc0, 0xc0, 0xc0, 0x66, 0x3c } },
    ['D'] = { { 0xf8, 0x6c, 0x66, 0x66, 0x66, 0x66, 0x6c, 0xf8 } },
    ['E'] = { { 0xfe, 0x62, 0x68, 0x78, 0x68, 0x62, 0x62, 0xfe } },
    ['F'] = { { 0xfe, 0x62, 0x68, 0x78, 0x68, 0x60, 0x60, 0xf0 } },
    ['G'] = { { 0x3c, 0x66, 0xc0, 0xc0, 0xce, 0xc6, 0x66, 0x3e } },
    ['H'] = { { 0x66, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x66, 0x66 } },
    ['I'] = { { 0x3c, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c } },
    ['J'] = { { 0x1e, 0x0c, 0x0c, 0x0c, 0x0c, 0xcc, 0xcc, 0x78 } },
    ['K'] = { { 0xe6, 0x66, 0x6c, 0x78, 0x78, 0x6c, 0x66, 0xe6 } },
    ['L'] = { { 0xf0, 0x60, 0x60, 0x60, 0x60, 0x62, 0x66, 0xfe } },
    ['M'] = { { 0xc6, 0xee, 0xfe, 0xd6, 0xc6, 0xc6, 0xc6, 0xc6 } },
    ['N'] = { { 0xc6, 0xe6, 0xf6, 0xde, 0xce, 0xc6, 0xc6, 0xc6 } },
    ['O'] = { { 0x3c, 0x66, 0xc6, 0xc6, 0xc6, 0xc6, 0x66, 0x3c } },
    ['P'] = { { 0xfc, 0x66, 0x66, 0x7c, 0x60, 0x60, 0x60, 0xf0 } },
    ['Q'] = { { 0x3c, 0x66, 0xc6, 0xc6, 0xc6, 0xd6, 0x66, 0x3c } },
    ['R'] = { { 0xfc, 0x66, 0x66, 0x7c, 0x6c, 0x66, 0x66, 0xe6 } },
    ['S'] = { { 0x7e, 0xc3, 0xc0, 0x7e, 0x03, 0x03, 0xc3, 0x7e } },
    ['T'] = { { 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18 } },
    ['U'] = { { 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7e, 0x3c } },
    ['V'] = { { 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x6c, 0x38, 0x10 } },
    ['W'] = { { 0xc6, 0xc6, 0xc6, 0xd6, 0xfe, 0xee, 0xc6, 0xc6 } },
    ['X'] = { { 0xc6, 0x6c, 0x38, 0x10, 0x38, 0x6c, 0xc6, 0xc6 } },
    ['Y'] = { { 0xc6, 0x6c, 0x38, 0x10, 0x10, 0x10, 0x10, 0x10 } },
    ['Z'] = { { 0xfe, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0xfe } },
};

static const struct glyph8x8 *glyph_for(char ch)
{
    if ((unsigned char)ch < sizeof(font_glyphs) / sizeof(*font_glyphs))
        return &font_glyphs[(unsigned char)ch];
    return &font_unknown;
}

unsigned fb_text_width(const char *s)
{
    unsigned n = 0;

    while (*s++)
        n++;
    return n * FB_FONT_W;
}

unsigned fb_draw_text(struct fb_canvas *cv, unsigned x, unsigned y,
                      const char *s, uint32_t fg, uint32_t bg)
{
    unsigned cx = x;

    if (!cv || !cv->frames || !s)
        return x;

    for (; *s; s++) {
        const struct glyph8x8 *g = glyph_for(*s);

        for (unsigned row = 0; row < FB_FONT_H; row++) {
            uint8_t bits = g->row[row];

            for (unsigned col = 0; col < FB_FONT_W; col++) {
                if (bits & (0x80u >> col)) {
                    fb_put_pixel(cv, cx + col, y + row, fg);
                } else if (bg) {
                    fb_put_pixel(cv, cx + col, y + row, bg);
                }
            }
        }
        cx += FB_FONT_W;
    }
    return cx;
}

