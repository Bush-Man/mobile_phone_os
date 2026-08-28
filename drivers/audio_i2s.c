/*
 * audio_i2s.c - I2S/DMIC codec scaffold (phase 13, item 71, target
 * hardware). Registers the i2s backend only when the board table
 * lists a codec (empty on QEMU, matching the buttons/pmic pattern);
 * the ops structure is where the I2S DMA + DMIC path plugs in.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "audio.h"
#include "platform.h"

static int i2s_open(struct audio_backend *self, bool capture)
{
    (void)self; (void)capture;
    return -1;                          /* no codec configured yet    */
}

static void i2s_close(struct audio_backend *self, bool capture)
{
    (void)self; (void)capture;
}

static int i2s_play(struct audio_backend *self, const int16_t *s,
                    unsigned n)
{
    (void)self; (void)s; (void)n;
    return -1;
}

static int i2s_rec(struct audio_backend *self, int16_t *s, unsigned n)
{
    (void)self; (void)s; (void)n;
    return -1;
}

static struct audio_backend i2s_be = {
    .name     = "i2s",
    .priority = 50,
    .caps     = AUDIO_CAP_PLAYBACK | AUDIO_CAP_CAPTURE,
    .open     = i2s_open,
    .close    = i2s_close,
    .play     = i2s_play,
    .rec      = i2s_rec,
};

/*
 * Boards call this from bring-up once the codec entry exists in
 * their table (see docs/PHASE_12.md buttons pattern).
 */
void audio_i2s_backend_register(void)
{
    audio_backend_register(&i2s_be);
}
