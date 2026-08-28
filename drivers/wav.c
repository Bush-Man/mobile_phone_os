/*
 * wav.c - minimal RIFF/WAVE parser for the playback path (phase 13,
 * milestone: play a WAV file). Supports 16-bit PCM, mono or stereo
 * (stereo is downmixed by dropping the right sample), any rate is
 * accepted (the null backend is rate-agnostic; the I2S backend
 * reconfigures from wav_info on boards).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "audio.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int wav_parse(const void *buf, unsigned len, struct wav_info *out)
{
    const uint8_t *p = buf;
    unsigned off;
    bool have_fmt = false;

    if (!buf || !out || len < 44u)
        return -1;
    if (memcmp(p, "RIFF", 4) || memcmp(&p[8], "WAVE", 4))
        return -1;

    memset(out, 0, sizeof(*out));
    off = 12u;

    while (off + 8u <= len) {
        uint32_t sz = rd32(&p[off + 4]);

        if (!memcmp(&p[off], "fmt ", 4) && sz >= 16u) {
            out->channels = rd16(&p[off + 10]);
            out->rate     = rd32(&p[off + 12]);
            out->bits     = rd16(&p[off + 22]);
            have_fmt      = true;
        } else if (!memcmp(&p[off], "data", 4)) {
            out->data_off   = off + 8u;
            out->data_bytes = sz;
            if (out->data_off + out->data_bytes > len)
                out->data_bytes = len - out->data_off;
            break;
        }
        off += 8u + sz + (sz & 1u);     /* chunks are word-aligned    */
    }

    if (!have_fmt || out->data_bytes == 0u)
        return -1;
    if (out->bits != 16u || out->channels == 0u || out->rate == 0u)
        return -1;
    return 0;
}
