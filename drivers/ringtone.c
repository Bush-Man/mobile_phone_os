/*
 * ringtone.c - procedural two-tone ringer (phase 13, item 73).
 *
 * The melody alternates 800 Hz / 1000 Hz square tones in 125 ms
 * steps with 62.5 ms gaps (classic cadence), generated frame-block
 * by frame-block into MIX_CH_RINGER via the HAL so ringer volume is
 * mixer-controlled like everything else. Ringer frames are pure
 * counters: playing() is asserted by the selftest.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "ringtone.h"

#define RT_STEP_FRAMES (AUDIO_RATE / 8u)    /* 125 ms per melody step  */
#define RT_BLOCK       320u                 /* 20 ms per tick          */

static struct {
    bool     playing;
    uint32_t pos;                       /* frames since start         */
    struct audio_stream *st;
} rt;

void ringtone_start(void)
{
    if (rt.playing)
        return;
    rt.st = audio_open_playback(MIX_CH_RINGER);
    if (!rt.st)
        return;
    rt.pos     = 0;
    rt.playing = true;
}

void ringtone_stop(void)
{
    if (!rt.playing)
        return;
    if (rt.st)
        audio_close(rt.st);
    rt.st      = NULL;
    rt.playing = false;
}

bool ringtone_playing(void)
{
    return rt.playing;
}

/* one 20 ms block of the melody                                   */
void ringtone_tick(uint64_t now_ms)
{
    int16_t buf[RT_BLOCK];
    unsigned step, i;

    (void)now_ms;
    if (!rt.playing || !rt.st)
        return;

    step = (rt.pos / RT_STEP_FRAMES) % 4u;

    for (i = 0; i < RT_BLOCK; i++) {
        uint32_t frame = rt.pos + i;

        if (step >= 2u) {
            buf[i] = 0;                 /* gap half of the cadence    */
            continue;
        }
        /* square tone: 800 Hz -> 20 samples/period at 16 k, flip   */
        /* every 10; 1000 Hz -> flip every 8                        */
        {
            uint32_t per = (step == 0u) ? 20u : 16u;
            uint32_t ph  = frame % per;

            buf[i] = (ph < per / 2u) ? 9000 : -9000;
        }
    }

    audio_write(rt.st, buf, RT_BLOCK);
    rt.pos += RT_BLOCK;
}
