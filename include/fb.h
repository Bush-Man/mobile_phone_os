#ifndef FB_H
#define FB_H

#include <stdbool.h>
#include <stdint.h>

#include "mm/types.h"

/*
 * Display subsystem (phase 9, plan item 48).
 *
 * Layering:
 *
 *   UI/test code          (draw calls below)
 *     -> fb core          (this header + drivers/fb.c: canvases,
 *                          span iteration, blitters, font)
 *       -> backends       (virtio-gpu in QEMU; DTB simple-framebuffer
 *                          slots in here later for phone boards)
 *
 * A canvas is N disjoint 4 KiB physical pages laid out row-major,
 * XRGB8888 only (stride == width * 4 today). Backends hand out the
 * page list; the core walks it through the uncached device-window
 * alias like the rest of the virtio stack, so writes reach the host
 * GPU coherently without cache maintenance anywhere.
 *
 * Double buffering is owned by the backend: when flip() != NULL the
 * core treats the canvas as a back buffer the backend presents on
 * demand (two guest resources alternating SET_SCANOUT under QEMU's
 * virtio-gpu). vsync/fences are not negotiated -- flip() returns
 * after the submit took effect transport-side; there is no frame
 * completion event (documented platform limitation).
 */

#define FB_FMT_XRGB8888 0x34325258u     /* DRM fourcc 'XR24'        */
#define FB_BPP          4u

#define FB_FONT_W       8u
#define FB_FONT_H       8u

/* compose a pixel value (0..255 components, X ignored)            */
static inline uint32_t fb_rgb(unsigned r, unsigned g, unsigned b)
{
    return 0xff000000u | ((r & 0xffu) << 16) |
           ((g & 0xffu) << 8) | (b & 0xffu);
}

/* ---- canvas ------------------------------------------------------------------ */

struct fb_canvas {
    const char *name;

    unsigned    width, height;
    unsigned    stride_bytes;

    unsigned    nframes;                /* PAGE_SIZE-sized pages      */
    paddr_t    *frames;                 /* backend-owned array        */

    bool        double_buffered;
    int       (*flip)(struct fb_canvas *cv);

    /* backend-private bookkeeping (resource ids, queues, ...)      */
    void       *priv;
};

/* logical size in bytes (stride * height; last line may end mid-page) */
static inline uint64_t fb_bytes(const struct fb_canvas *cv)
{
    return (uint64_t)cv->stride_bytes * cv->height;
}

/* ---- backend registry ------------------------------------------------------------ */

typedef int (*fb_claim_fn)(struct fb_canvas *out);

struct fb_backend {
    const char *name;
    int         priority;               /* higher wins claim fight    */
    fb_claim_fn claim;

    struct fb_backend *next;
};

int fb_backend_register(const struct fb_backend *b);

/*
 * Ask registered backends, highest priority first, until one hands
 * out a live canvas. Returns 0 / -ENODEV. Success owns nothing but
 * the pointer -- canvases stay alive for the session.
 */
int fb_claim_default(struct fb_canvas *out);

bool fb_present(void);                  /* any claim succeeded once   */
const struct fb_canvas *fb_active(void);

/* ---- pixels ---------------------------------------------------------------------- */

void fb_fill_rect(struct fb_canvas *cv, unsigned x, unsigned y,
                  unsigned w, unsigned h, uint32_t c);

/*
 * Copy a rectangle between arbitrary canvases (or within one --
 * overlapping regions are snapshot line-by-line first). All
 * rectangles clamp to their canvas bounds.
 */
void fb_copy_rect(struct fb_canvas *dst, unsigned dx, unsigned dy,
                  const struct fb_canvas *src, unsigned sx, unsigned sy,
                  unsigned w, unsigned h);

/* alpha blend src over dst (a = coverage 0..255)                    */
void fb_blend_rect(struct fb_canvas *dst, unsigned dx, unsigned dy,
                   const struct fb_canvas *src, unsigned sx, unsigned sy,
                   unsigned w, unsigned h, uint8_t a);

void fb_put_pixel(struct fb_canvas *cv, unsigned x, unsigned y,
                  uint32_t c);

/* deterministic FNV-1a over a clamped region (selftest uses it to
 * prove write/read consistency of backend memory end to end)       */
uint32_t fb_region_hash(const struct fb_canvas *cv, unsigned x,
                        unsigned y, unsigned w, unsigned h);

/* ---- text ------------------------------------------------------------------------- */

/*
 * Minimal built-in 8x8 bitmap font. The glyph table covers the
 * characters the on-screen test banners need (uppercase letters,
 * digits, '.', '-', '#', ':', '/'); anything else renders as a
 * solid block placeholder so no call site can crash or vanish.
 */
unsigned fb_text_width(const char *s);  /* in pixels, incl spacing   */

/* bg == 0 draws glyphs only (transparent); returns next x          */
unsigned fb_draw_text(struct fb_canvas *cv, unsigned x, unsigned y,
                      const char *s, uint32_t fg, uint32_t bg);

#endif /* FB_H */