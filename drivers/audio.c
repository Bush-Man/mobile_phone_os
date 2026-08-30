/*
 * audio.c - HAL core: backend registry, streams, mixer, the call-
 * routing seam and the modem PCM hooks (phase 13, items 70+72).
 *
 * Volume is applied per mixer channel on every write/read with the
 * pure integer scale ((s*vol+127)/255) so attenuation is verified
 * by inspecting backend taps rather than by ear. Call routing
 * consumes the phase-12 seam (call_audio_route_set): CALL_ACTIVE
 * moves the call channels onto the modem PCM hooks, INCOMING
 * starts the ringtone, IDLE stops it. The modem hooks themselves
 * are the PCM bus seam: on boards they bind to the modem PCM
 * config; on QEMU a loopback sink/source pair proves both
 * directions byte-wise.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "audio.h"
#include "lib.h"
#include "modem.h"
#include "ringtone.h"
#include "spinlock.h"
#include "time.h"

struct audio_backend *backends;
static unsigned nbackends;

static struct {
    uint8_t vol[MIX_CH_MAX];
    bool    call_routed;
    uint64_t played_frames;
    spinlock_t lock;
} am = {
    .vol  = { 255u, 255u, 255u, 200u },
    .lock = SPINLOCK_INIT,
};

int16_t mixer_scale(int16_t s, uint8_t vol)
{
    int32_t p = (int32_t)s * vol;

    /* round half away from zero so +x and -x scale symmetrically */
    return (int16_t)(p >= 0 ? (p + 127) / 255 : -((-p + 127) / 255));
}

int mixer_set_volume(unsigned ch, uint8_t vol)
{
    if (ch >= MIX_CH_MAX)
        return -1;
    am.vol[ch] = vol;
    return 0;
}

uint8_t mixer_get_volume(unsigned ch)
{
    return ch < MIX_CH_MAX ? am.vol[ch] : 0u;
}

/* ---- backend registry ---------------------------------------------------------- */

int audio_backend_register(struct audio_backend *b)
{
    daif_state s;

    if (!b || !b->name || !b->open)
        return -1;

    spin_lock_irqsave(&am.lock, &s);
    b->next = backends;
    backends = (struct audio_backend *)b;
    nbackends++;
    spin_unlock_irqrestore(&am.lock, s);
    return 0;
}

unsigned audio_backend_count(void)
{
    return nbackends;
}

/* ---- streams ------------------------------------------------------------------------ */

enum stream_kind { ST_PLAY, ST_CAP };

struct audio_stream {
    bool            in_use;
    enum stream_kind kind;
    unsigned        ch;
    struct audio_backend *be;
};

static struct audio_stream streams[AUDIO_STREAM_MAX];

static struct audio_stream *stream_alloc(enum stream_kind kind,
                                         unsigned ch)
{
    for (unsigned i = 0; i < AUDIO_STREAM_MAX; i++)
        if (!streams[i].in_use) {
            streams[i].in_use = true;
            streams[i].kind   = kind;
            streams[i].ch     = ch;
            return &streams[i];
        }
    return NULL;
}

/*
 * Pick the highest-priority backend with `cap` whose priority is
 * below `ceiling` (UINT_MAX for the first attempt), so callers can
 * walk the list downwards when a backend refuses to open.
 */
static struct audio_backend *backend_below(unsigned cap,
                                           int ceiling)
{
    struct audio_backend *best = NULL;

    for (struct audio_backend *b = backends; b; b = b->next) {
        if (!(b->caps & cap))
            continue;
        if (ceiling >= 0 && b->priority >= ceiling)
            continue;
        if (!best || b->priority > best->priority)
            best = b;
    }
    return best;
}

/*
 * A high-priority backend that refuses open() (the i2s scaffold
 * without a codec, for instance) must not mute the whole HAL: fall
 * through to the next one down.
 */
static struct audio_backend *backend_open(unsigned cap, bool capture)
{
    int ceiling = -1;

    for (struct audio_backend *b = backend_below(cap, ceiling);
         b; b = backend_below(cap, ceiling)) {
        if (b->open(b, capture) == 0)
            return b;
        ceiling = b->priority;
    }
    return NULL;
}

struct audio_stream *audio_open_playback(unsigned ch)
{
    struct audio_backend *be = backend_open(AUDIO_CAP_PLAYBACK, false);
    struct audio_stream *st;

    if (!be)
        return NULL;
    st = stream_alloc(ST_PLAY, ch);
    if (!st) {
        be->close(be, false);
        return NULL;
    }
    st->be = be;
    return st;
}

struct audio_stream *audio_open_capture(unsigned ch)
{
    struct audio_backend *be = backend_open(AUDIO_CAP_CAPTURE, true);
    struct audio_stream *st;

    if (!be)
        return NULL;
    st = stream_alloc(ST_CAP, ch);
    if (!st) {
        be->close(be, true);
        return NULL;
    }
    st->be = be;
    return st;
}

void audio_close(struct audio_stream *st)
{
    if (!st || !st->in_use)
        return;
    st->be->close(st->be, st->kind == ST_CAP);
    st->in_use = false;
}

int audio_write(struct audio_stream *st, const int16_t *samples,
                unsigned n)
{
    int16_t scaled[256];
    unsigned done = 0;
    uint8_t vol;

    if (!st || st->kind != ST_PLAY)
        return -1;
    vol = am.vol[st->ch];

    while (done < n) {
        unsigned chunk = n - done < 256u ? n - done : 256u;

        if (vol == 255u) {
            if (st->be->play(st->be, samples + done, chunk) < 0)
                break;
        } else {
            for (unsigned i = 0; i < chunk; i++)
                scaled[i] = mixer_scale(samples[done + i], vol);
            if (st->be->play(st->be, scaled, chunk) < 0)
                break;
        }
        done += chunk;
        am.played_frames += chunk;
    }
    return (int)done;
}

int audio_read(struct audio_stream *st, int16_t *samples, unsigned n)
{
    if (!st || st->kind != ST_CAP)
        return -1;
    return st->be->rec(st->be, samples, n);
}

unsigned audio_played_frames(void)
{
    return (unsigned)am.played_frames;
}

/* ---- call routing seam (item 72) + modem PCM hooks ------------------------------ */

static struct {
    /* DL: frames written toward the network (modem bus sink).     */
    int16_t  dl_tap[512];
    unsigned dl_len;
    /* UL: frames produced by the modem toward the app (loopback
     * source on QEMU: a fixed marker tone).                        */
    int16_t  ul_ring[512];
    unsigned ul_head, ul_count;
    bool     call_pcm_active;
} pcm;

int audio_call_dl_write(const int16_t *samples, unsigned n)
{
    if (!pcm.call_pcm_active)
        return -1;
    if (n > 512u)
        n = 512u;
    memcpy(pcm.dl_tap, samples, n * sizeof(int16_t));
    pcm.dl_len = n;
    return (int)n;
}

int audio_call_ul_read(int16_t *samples, unsigned n)
{
    unsigned take = pcm.ul_count < n ? pcm.ul_count : n;

    for (unsigned i = 0; i < take; i++) {
        samples[i] = pcm.ul_ring[pcm.ul_head];
        pcm.ul_head = (pcm.ul_head + 1u) % 512u;
        pcm.ul_count--;
    }
    return (int)take;
}

/* fill the UL ring with the uplink marker tone (QEMU loopback)    */
static void pcm_ul_generate(unsigned n)
{
    static uint32_t phase;

    for (unsigned i = 0; i < n && pcm.ul_count < 512u; i++) {
        /* 600 Hz square marker: sign flips every 13/14 frames      */
        pcm.ul_ring[(pcm.ul_head + pcm.ul_count) % 512u] =
            (phase++ % 27u) < 13u ? 6000 : -6000;
        pcm.ul_count++;
    }
}

static void audio_on_route(enum call_state state, void *arg)
{
    /*
     * `state` is the post-transition call state delivered by the
     * phase-12 callctl seam.
     */
    (void)arg;
    if (state == CALL_ACTIVE) {
        pcm.call_pcm_active = true;
        pcm_ul_generate(128);
        kprintf("audio: call route ACTIVE (pcm dl/ul live)\n");
    } else if (state == CALL_INCOMING) {
        ringtone_start();
        kprintf("audio: incoming -- ringer on\n");
    } else if (state == CALL_IDLE) {
        pcm.call_pcm_active = false;
        ringtone_stop();
        kprintf("audio: call route IDLE\n");
    }
}

/* ---- tick + subsys init -------------------------------------------------------------- */

static bool audio_inited;

void audio_tick(uint64_t now_ms)
{
    (void)now_ms;
    /* ringer frames are synthesized by ringtone.c via the HAL     */
    ringtone_tick(now_ms);
}

void audio_subsys_init(const struct platform_info *plat)
{
    (void)plat;
    if (audio_inited)
        return;
    audio_inited = true;

    call_audio_route_set(audio_on_route, NULL);
    kprintf("audio: HAL online (%u backends), call seam armed\n",
            nbackends);
}

