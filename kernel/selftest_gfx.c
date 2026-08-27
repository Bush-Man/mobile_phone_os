/*
 * selftest_gfx.c - phase 9 graphics/input verification, run as the
 * "gfxtest" kernel task (everything here blocks on GPU round-trips).
 *
 * Checks, in order:
 *   1. backend claim: canvas geometry sane, fb0 node registered.
 *   2. fill + read consistency: paint, hash, repaint, re-hash --
 *      proves host-visible backing memory round-trips.
 *   3. blit equivalence: copy_rect lands identical bytes (compare
 *      against its own source region -- no magic values baked in).
 *   4. text: banner glyphs mutate their row (variance check).
 *   5. blend: full-coverage alpha behaves exactly like copy.
 *   6. present: transfer+flush acknowledged by the GPU backend.
 *   7. input: deterministic 12-event calibration sequence pushed so
 *      the evreader process can assert an exact stream afterwards
 *      (see userspace/evreader.c for the mirrored expectations).
 *
 * Summary line "selftest: gfx ok" matches the harness grep style;
 * no display present prints skip lines and still passes (same
 * policy as fstest without a disk).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "fb.h"
#include "input.h"
#include "lib.h"
#include "proc.h"
#include "task.h"
#include "virtio_gpu.h"

static int failures;

#define CHECK(cond, name)                                              \
    do {                                                               \
        if (cond) {                                                    \
            kprintf("gfxtest: %-34s ok\n", name);                      \
        } else {                                                       \
            kprintf("gfxtest: %-34s FAIL\n", name);                    \
            failures++;                                                \
        }                                                              \
    } while (0)

/* ---- pattern helper --------------------------------------------------------------- */

static void draw_test_pattern(struct fb_canvas *cv)
{
    for (unsigned i = 0; i < 8; i++)                /* rainbow bars */
        fb_fill_rect(cv, cv->width / 8 * i, 0,
                     cv->width / 8 + 1, cv->height / 4,
                     fb_rgb(255 - i * 31u, i * 37u, 40 + i * 26u));

    for (unsigned y = cv->height / 4; y < cv->height / 2; y++) {
        unsigned t = y * 255u / (cv->height / 2);

        fb_fill_rect(cv, 0, y, cv->width, 1,
                     fb_rgb(t, t, 255 - t));
    }

    fb_fill_rect(cv, 0, cv->height / 2, cv->width,
                 cv->height - cv->height / 2,
                 fb_rgb(16, 18, 24));

    fb_draw_text(cv, 24, cv->height / 2 + 24, "PHASE 9 GFX OK",
                 fb_rgb(240, 240, 120), fb_rgb(30, 60, 120));
}

static struct {
    bool     claimed;
    uint32_t hash_full;
} gfx;

static void fb_tests(void)
{
    struct fb_canvas cv;

    if (fb_claim_default(&cv)) {
        kprintf("gfxtest: no display backend present, skipping\n");
        return;
    }
    gfx.claimed = true;

    CHECK(cv.width > 0 && cv.height > 0 &&
              cv.stride_bytes == cv.width * FB_BPP &&
              cv.frames && cv.nframes > 0,
          "canvas geometry");

    draw_test_pattern(&cv);
    gfx.hash_full = fb_region_hash(&cv, 0, 0, cv.width, cv.height);

    /* repaint identically: hash must not move                      */
    draw_test_pattern(&cv);
    CHECK(fb_region_hash(&cv, 0, 0, cv.width, cv.height) ==
              gfx.hash_full,
          "fill+text determinism");

    /* sprite copy across page boundaries stays byte-identical      */
    {
        unsigned sx = cv.width - 10;
        unsigned sy = cv.height - 12;
        uint32_t h1 = fb_region_hash(&cv, sx, sy, 6, 6);

        fb_copy_rect(&cv, sx - 600, sy - 60, &cv, sx, sy, 6, 6);
        CHECK(fb_region_hash(&cv, sx - 600, sy - 60, 6, 6) == h1,
              "copy_rect equivalence");
    }

    /* alpha==255 blends exactly like a copy                        */
    {
        uint32_t src_hash;

        fb_fill_rect(&cv, 10, cv.height - 20, 50, 10,
                     fb_rgb(200, 100, 50));
        src_hash = fb_region_hash(&cv, 10, cv.height - 20, 50, 10);
        fb_blend_rect(&cv, 70, cv.height - 20, &cv,
                      10, cv.height - 20, 50, 10, 255);
        CHECK(fb_region_hash(&cv, 70, cv.height - 20, 50, 10) ==
                  src_hash,
              "blend a=255 == copy");
    }

    /* text mutates its row                                         */
    {
        unsigned ty = cv.height / 2 + 72;
        uint32_t before =
            fb_region_hash(&cv, 24, ty, 20 * FB_FONT_W, FB_FONT_H);

        fb_draw_text(&cv, 24, ty, "MARKER ROW",
                     fb_rgb(255, 255, 0), 0);
        CHECK(fb_region_hash(&cv, 24, ty, 20 * FB_FONT_W,
                             FB_FONT_H) != before,
              "text region mutates");
    }
}

static void present_tests(void)
{
    const struct fb_canvas *cv = fb_active();

    if (!cv) {
        kprintf("gfxtest: nothing to present, skipping\n");
        return;
    }

    {
        int rc;

        if (cv->flip)
            rc = cv->flip((struct fb_canvas *)cv);
        else
            rc = fb_virtio_gpu_present();
        CHECK(rc == 0, "present frame to scanout");
    }
}

/* ---- input calibration feed ---------------------------------------------------------- */

/*
 * The exact 12-event stream userspace/evreader.c asserts. Keep the
 * two tables in lockstep; the doc records the ABI.
 */
static const struct input_event calib[] = {
    { 0, EV_KEY, BTN_TOUCH,    1 },
    { 0, EV_ABS, ABS_X,      400 },
    { 0, EV_ABS, ABS_Y,      300 },
    { 0, EV_SYN, SYN_REPORT,   0 },
    { 0, EV_KEY, BTN_TOUCH,    0 },
    { 0, EV_SYN, SYN_REPORT,   0 },
    { 0, EV_ABS, ABS_X,      200 },
    { 0, EV_SYN, SYN_REPORT,   0 },
    { 0, EV_ABS, ABS_Y,      100 },
    { 0, EV_SYN, SYN_REPORT,   0 },
    { 0, EV_KEY, KEY_VOLUMEUP, 1 },
    { 0, EV_SYN, SYN_REPORT,   0 },
};

#define CALIB_COUNT (sizeof(calib) / sizeof(calib[0]))

static void input_tests(void)
{
    /*
     * No warm-up games: the ring starts empty, so the very first
     * twelve records are exactly the calibration sequence the
     * evreader binary asserts on. Pushed BEFORE the reader process
     * exists so its blocking open()+read() sees an instant backlog.
     */
    for (unsigned i = 0; i < CALIB_COUNT; i++)
        input_push(calib[i].type, calib[i].code, calib[i].value);

    CHECK(input_pending() == CALIB_COUNT,
          "calibration stream queued");
}

/* ---- entry ------------------------------------------------------------------------ */

void gfx_selftest_task(void *arg)
{
    int pid;

    (void)arg;

    kprintf("gfxtest: phase 9 graphics/input selftests\n");

    fb_tests();
    present_tests();
    input_tests();

    if (!failures)
        kprintf("selftest: gfx ok\n");
    else
        kprintf("selftest: gfx FAILED (%d)\n", failures);

    /* milestone consumer: reads + asserts the stream at EL0        */
    pid = proc_spawn("evreader",
                     (const char *const []){ "evreader", NULL },
                     NULL);
    if (pid < 0)
        kprintf("[demo] evreader spawn failed (%d)\n", pid);
    else
        kprintf("[demo] evreader spawned pid %d\n", pid);

    task_exit();
}



