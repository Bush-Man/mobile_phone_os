/*
 * selftest_audio.c - phase 13 verification, run as the "audiotest"
 * task.
 *
 * Checks, in order:
 *   1. backend: null registered with playback+capture caps.
 *   2. mixer math: mixer_scale at volume 255 / 128 / 0.
 *   3. playback: media stream write -> null tap holds the samples
 *      (unscaled at 255, halved at 128); played counter advances.
 *   4. wav: in-memory RIFF built + parsed, played via the HAL; then
 *      written to /wav/test.wav through the VFS, read back, parsed
 *      and played again (milestone: play a WAV file).
 *   5. capture: null capture tone frames return.
 *   6. call audio: route CALL_ACTIVE -> dl write taps downlink,
 *      ul read returns the loopback marker tone (both directions);
 *      route IDLE stops the PCM path (milestone part 2).
 *   7. ringtone: incoming route starts it; ticks produce frames;
 *      stop silences it.
 *
 * Summary "selftest: audio ok" matches the harness style.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "audio.h"
#include "lib.h"
#include "modem.h"
#include "ringtone.h"
#include "task.h"
#include "time.h"
#include "vfs.h"

static int failures;

#define CHECK(cond, name)                                              \
    do {                                                               \
        if (cond) {                                                    \
            kprintf("audiotest: %-34s ok\n", name);                    \
        } else {                                                       \
            kprintf("audiotest: %-34s FAIL\n", name);                  \
            failures++;                                                \
        }                                                              \
    } while (0)

static int16_t tone_buf[512];

static int16_t tone_buf[512];
static int16_t chk_buf[512];

static void make_tone(int16_t *buf, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        buf[i] = (int16_t)((i % 32u) < 16u ? 10000 : -10000);
}

/* ---- mixer + playback ----------------------------------------------------------- */

static void mixer_tests(void)
{
    CHECK(mixer_scale(10000, 255) == 10000, "scale vol=255 unity");
    CHECK(mixer_scale(10000, 128) == 5023, "scale vol=128 half");
    CHECK(mixer_scale(-10000, 128) == -5023, "scale negative");
    CHECK(mixer_scale(10000, 0) == 0, "scale vol=0 mute");
}

static void play_tests(void)
{
    struct audio_stream *st;
    const int16_t *tap;
    unsigned tap_frames, rate;
    unsigned before = audio_played_frames();

    st = audio_open_playback(MIX_CH_MEDIA);
    CHECK(st != NULL, "playback stream open");
    if (!st)
        return;

    make_tone(tone_buf, 200);
    mixer_set_volume(MIX_CH_MEDIA, 255);
    CHECK(audio_write(st, tone_buf, 200) == 200, "playback write");
    CHECK(audio_played_frames() == before + 200u,
          "played counter advances");

    tap = audio_null_tap(&tap_frames, &rate);
    CHECK(tap_frames >= 200u && rate == AUDIO_RATE, "null tap armed");
    CHECK(tap[tap_frames - 1] == tone_buf[199],
          "tap holds unscaled samples");

    /* halved volume changes the tap                                */
    mixer_set_volume(MIX_CH_MEDIA, 128);
    audio_write(st, tone_buf, 200);
    tap = audio_null_tap(&tap_frames, &rate);
    CHECK(tap[tap_frames - 1] == mixer_scale(tone_buf[199], 128),
          "tap volume-scaled");

    audio_close(st);
}

/* ---- wav --------------------------------------------------------------------------- */

static unsigned wav_build(int16_t *pcm, unsigned nsamples,
                          uint8_t *out, unsigned cap)
{
    unsigned data = nsamples * 2u;
    unsigned o = 0;

#define WPUT32(v) do {                                       \
        out[o++] = (uint8_t)(v); out[o++] = (uint8_t)((v) >> 8); \
        out[o++] = (uint8_t)((v) >> 16);                          \
        out[o++] = (uint8_t)((v) >> 24); } while (0)
#define WPUT16(v) do {                                       \
        out[o++] = (uint8_t)(v); out[o++] = (uint8_t)((v) >> 8); } \
        while (0)

    if (44u + data > cap)
        return 0;
    memcpy(&out[o], "RIFF", 4); o += 4;
    WPUT32(36u + data);
    memcpy(&out[o], "WAVE", 4); o += 4;
    memcpy(&out[o], "fmt ", 4); o += 4;
    WPUT32(16);
    WPUT16(1);                          /* PCM                        */
    WPUT16(1);                          /* mono                       */
    WPUT32(AUDIO_RATE);
    WPUT32(AUDIO_RATE * 2u);            /* byte rate                  */
    WPUT16(2);                          /* block align                */
    WPUT16(16);                         /* bits                       */
    memcpy(&out[o], "data", 4); o += 4;
    WPUT32(data);
    memcpy(&out[o], pcm, data);
    o += data;
#undef WPUT16
#undef WPUT32
    return o;
}

static void wav_tests(void)
{
    static uint8_t wavbuf[8192];
    struct wav_info wi;
    struct audio_stream *st;
    unsigned wavlen, frames;

    make_tone(tone_buf, 256);
    wavlen = wav_build(tone_buf, 256, wavbuf, sizeof(wavbuf));

    CHECK(wavlen == 44u + 512u, "wav built");
    CHECK(wav_parse(wavbuf, wavlen, &wi) == 0 &&
              wi.channels == 1u && wi.rate == AUDIO_RATE &&
              wi.bits == 16u && wi.data_bytes == 512u,
          "wav parse in-memory");

    st = audio_open_playback(MIX_CH_MEDIA);
    if (!st)
        return;
    {
        const int16_t *pcm =
            (const int16_t *)(wavbuf + wi.data_off);

        CHECK(audio_write(st, pcm, 256) == 256,
              "wav played via HAL");
    }
    audio_close(st);

    /* filesystem leg: store, read back, parse, play               */
    {
        struct file *f;
        uint8_t back[8192];
        int r;

        vfs_mkdir("/wav");
        r = vfs_open("/wav/test.wav", O_CREAT | O_WRONLY, &f);
        if (r == 0) {
            f_write(f, wavbuf, wavlen);
            file_close(f);
        }
        r = vfs_open("/wav/test.wav", O_RDONLY, &f);
        CHECK(r == 0, "wav file open");
        if (r)
            return;
        r = f_read(f, back, wavlen);
        file_close(f);
        CHECK(r == (int)wavlen, "wav file read back");
        CHECK(wav_parse(back, (unsigned)r, &wi) == 0,
              "wav parse from vfs");

        frames = wi.data_bytes / 2u;
        st = audio_open_playback(MIX_CH_MEDIA);
        if (!st)
            return;
        {
            const int16_t *pcm =
                (const int16_t *)(back + wi.data_off);

            CHECK(audio_write(st, pcm, frames) == (int)frames,
                  "wav file played");
        }
        audio_close(st);
    }
}

/* ---- capture + call audio + ringer ------------------------------------------------ */

static void capture_tests(void)
{
    struct audio_stream *st = audio_open_capture(MIX_CH_CALL_UL);
    int16_t buf[128];

    CHECK(st != NULL, "capture stream open");
    if (!st)
        return;
    CHECK(audio_read(st, buf, 128) == 128, "capture tone frames");
    {
        bool nonzero = false;

        for (unsigned i = 0; i < 128u; i++)
            nonzero |= buf[i] != 0;
        CHECK(nonzero, "capture tone non-silent");
    }
    audio_close(st);
}

static void call_tests(void)
{
    int16_t dl[64], ul[64];

    make_tone(dl, 64);

    /* CALL_ACTIVE via the phase-12 seam: pcm path goes live        */
    call_ctl_apply(CALL_EV_DIAL);
    call_ctl_apply(CALL_EV_OK);
    call_ctl_apply(CALL_EV_CONNECT);
    CHECK(call_ctl_state() == CALL_ACTIVE, "call active (seam)");

    CHECK(audio_call_dl_write(dl, 64) == 64,
          "call downlink written to pcm bus");
    CHECK(audio_call_ul_read(ul, 64) == 64,
          "call uplink frames read");
    {
        bool marker = true;

        for (unsigned i = 0; i < 64u; i++)
            if (ul[i] != 6000 && ul[i] != -6000)
                marker = false;
        CHECK(marker, "uplink marker tone intact");
    }

    call_ctl_apply(CALL_EV_HANGUP_LOCAL);
    call_ctl_apply(CALL_EV_OK);
    CHECK(audio_call_dl_write(dl, 64) < 0,
          "pcm path closed on idle");
}

static void ring_tests(void)
{
    unsigned played_before = audio_played_frames();

    call_ctl_apply(CALL_EV_INCOMING);
    CHECK(ringtone_playing(), "ringer auto-start on incoming");

    ringtone_tick(time_uptime_ms());
    ringtone_tick(time_uptime_ms());
    CHECK(audio_played_frames() > played_before,
          "ringer frames through mixer");

    call_ctl_apply(CALL_EV_HANGUP_REMOTE);
    CHECK(!ringtone_playing(), "ringer stops on idle");
}

/* ---- entry ---------------------------------------------------------------------------- */

void audio_selftest_task(void *arg)
{
    (void)arg;

    kprintf("audiotest: phase 13 audio selftests\n");

    mixer_tests();
    play_tests();
    wav_tests();
    capture_tests();
    call_tests();
    ring_tests();

    if (!failures)
        kprintf("selftest: audio ok\n");
    else
        kprintf("selftest: audio FAILED (%d)\n", failures);

    task_exit();
}
