#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

struct platform_info;
struct netif;   /* not used here; keeps forward decls tidy */

/*
 * Audio HAL (phase 13, plan items 70-73).
 *
 * Format contract for the whole subsystem: mono, signed 16-bit LE
 * PCM at AUDIO_RATE. Streams belong to a mixer channel; every write
 * passes through the channel volume ((s*vol+127)/255) before
 * reaching the backend, so routing and attenuation are testable as
 * pure integer math on the tapped output.
 *
 * Backends:
 *   - null (QEMU fallback per item 71; virtio-sound needs virtio 1.0
 *     which our legacy mmio transport cannot offer -- documented):
 *     playback lands in a discard ring with a last-frame
 *     verification tap; capture generates a procedural tone so the
 *     capture path is exercisable.
 *   - i2s (boards; scaffold registers only when the board table
 *     lists a codec).
 *
 * Call routing (item 72) consumes the phase-12 seam: audio_on_route
 * is registered via call_audio_route_set(); CALL_ACTIVE routes the
 * call channels to the modem PCM hooks (HW: PCM bus config;
 * QEMU: modem-mock PCM loopback), CALL_INCOMING starts the
 * ringtone (item 73), back to IDLE stops it.
 */

#define AUDIO_RATE      16000u
#define AUDIO_CHANNELS  1u              /* mono                       */

#define MIX_CH_MEDIA    0u              /* wav / notifications etc.   */
#define MIX_CH_CALL_DL  1u              /* downlink (network->spk)    */
#define MIX_CH_CALL_UL  2u              /* uplink (mic->network)      */
#define MIX_CH_RINGER   3u
#define MIX_CH_MAX      4u

/* ---- backend registry ---------------------------------------------------------- */

#define AUDIO_CAP_PLAYBACK (1u << 0)
#define AUDIO_CAP_CAPTURE  (1u << 1)

struct audio_backend {
    const char *name;
    int         priority;
    unsigned    caps;                   /* AUDIO_CAP_*                */

    int  (*open)(struct audio_backend *self, bool capture);
    void (*close)(struct audio_backend *self, bool capture);
    /* n = frames (samples); returns frames accepted               */
    int  (*play)(struct audio_backend *self, const int16_t *samples,
                 unsigned n);
    int  (*rec)(struct audio_backend *self, int16_t *samples,
                unsigned n);

    struct audio_backend *next;
};

int  audio_backend_register(const struct audio_backend *b);
unsigned audio_backend_count(void);

/* ---- HAL -------------------------------------------------------------------------- */

#define AUDIO_STREAM_MAX 8u

struct audio_stream;

struct audio_stream *audio_open_playback(unsigned mixer_ch);
struct audio_stream *audio_open_capture(unsigned mixer_ch);
void audio_close(struct audio_stream *st);
/* n = frames; volume applied per mixer channel                    */
int  audio_write(struct audio_stream *st, const int16_t *samples,
                 unsigned n);
int  audio_read(struct audio_stream *st, int16_t *samples,
                unsigned n);

unsigned audio_played_frames(void);     /* HAL-wide counter           */

/* mixer                                                                            */
int  mixer_set_volume(unsigned ch, uint8_t vol);    /* 0..255        */
uint8_t mixer_get_volume(unsigned ch);
/* pure scaling used everywhere ((s*vol+127)/255)                  */
int16_t mixer_scale(int16_t s, uint8_t vol);

/* ---- wav --------------------------------------------------------------------------- */

struct wav_info {
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;                      /* 16 supported               */
    uint32_t data_off;                  /* offset of PCM data         */
    uint32_t data_bytes;
};

int  wav_parse(const void *buf, unsigned len, struct wav_info *out);

/* ---- call routing seam + ringtone ---------------------------------------------------- */

void audio_subsys_init(const struct platform_info *plat);
void audio_tick(uint64_t now_ms);       /* housekeeping cadence       */

/* modem PCM hooks (HW: PCM bus; QEMU: modem-mock loopback)         */
int  audio_call_dl_write(const int16_t *samples, unsigned n);
int  audio_call_ul_read(int16_t *samples, unsigned n);

void ringtone_start(void);
void ringtone_stop(void);
bool ringtone_playing(void);
/* generator: fills n frames of the two-tone melody from pos       */
void ringtone_generate(int16_t *out, unsigned n, uint32_t pos);

/* notifications (item 73): queued through the ringer channel       */
void audio_notify_beep(void);

/* null backend verification tap (selftests)                        */
const int16_t *audio_null_tap(unsigned *frames_out, unsigned *rate_out);
unsigned audio_null_play_frames(void);

#endif /* AUDIO_H */