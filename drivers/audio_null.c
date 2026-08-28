/*
 * audio_null.c - QEMU fallback backend (phase 13, item 71).
 *
 * The plan allows null/virtio-sound; virtio-sound requires virtio
 * 1.0 which the legacy mmio transport cannot negotiate, so the
 * fallback is this null sink: playback frames land in a discard
 * counter plus a last-frame verification tap (selftests inspect it
 * instead of ears), and capture synthesizes a fixed 1 kHz tone so
 * the capture path is exercisable. Real capture is I2S/DMIC.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "audio.h"

#define NULL_TAP 256u

static struct {
    bool     play_open, cap_open;
    uint64_t played;
    int16_t  tap[NULL_TAP];
    unsigned tap_len;
    uint32_t cap_phase;
} nb;

static int nb_open(struct audio_backend *self, bool capture)
{
    (void)self;
    if (capture) {
        if (nb.cap_open)
            return -1;
        nb.cap_open = true;
    } else {
        if (nb.play_open)
            return -1;
        nb.play_open = true;
    }
    return 0;
}

static void nb_close(struct audio_backend *self, bool capture)
{
    (void)self;
    if (capture)
        nb.cap_open = false;
    else
        nb.play_open = false;
}

static int nb_play(struct audio_backend *self, const int16_t *s,
                   unsigned n)
{
    (void)self;
    nb.played += n;

    /* verification tap: keep the last written frames               */
    if (n > NULL_TAP)
        s += n - NULL_TAP, n = NULL_TAP;
    memcpy(nb.tap, s, n * sizeof(int16_t));
    nb.tap_len = n;
    return (int)n;
}

static int nb_rec(struct audio_backend *self, int16_t *s, unsigned n)
{
    (void)self;
    /* procedural 1 kHz capture tone (50-frame half period)         */
    for (unsigned i = 0; i < n; i++)
        s[i] = (nb.cap_phase++ % 32u) < 16u ? 8000 : -8000;
    return (int)n;
}

static struct audio_backend null_be = {
    .name     = "null",
    .priority = 10,
    .caps     = AUDIO_CAP_PLAYBACK | AUDIO_CAP_CAPTURE,
    .open     = nb_open,
    .close    = nb_close,
    .play     = nb_play,
    .rec      = nb_rec,
};

void audio_null_backend_register(void)
{
    audio_backend_register(&null_be);
}

const int16_t *audio_null_tap(unsigned *frames_out, unsigned *rate_out)
{
    *frames_out = nb.tap_len;
    *rate_out   = AUDIO_RATE;
    return nb.tap;
}

unsigned audio_null_play_frames(void)
{
    return (unsigned)nb.played;
}
