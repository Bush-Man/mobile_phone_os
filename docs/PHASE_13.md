# Phase 13 — Audio (Implementation Log)

Milestone scope reached: the audio HAL with backend registry,
playback/capture streams and a per-channel integer mixer
(include/audio.h + drivers/audio.c, item 70); the QEMU fallback
null backend with a last-frame verification tap and procedural
capture tone plus the I2S/DMIC board scaffold
(drivers/audio_null.c audio_i2s.c, item 71 -- virtio-sound
requires virtio 1.0 which the legacy mmio transport cannot
negotiate, so the plan's "null" fallback is the delivered path);
the phase-12 call-routing seam consumed: CALL_ACTIVE moves the
call channels onto the modem PCM hooks (audio_call_dl_write /
audio_call_ul_read, QEMU loopback sink/source pair; PCM bus
config documented for boards, item 72); and the procedural
two-tone ringtone auto-started on incoming calls with a
notification beep API (drivers/ringtone.c, item 73). Milestone
delivery in audiotest: an in-memory WAV built, parsed by the RIFF
parser, played through the mixer with tap-verified scaling, then
written to /wav/test.wav via the VFS, read back, parsed and
played again; a live call route puts CALL_ACTIVE through the
seam with downlink writes tapped and uplink marker frames read
back -- "call audio both directions". Per the standing
coordination decision no make target ran; all 8 new + 1 edited
units passed the per-file `-fsyntax-only` sweeps.

## Order of files written

### Batch A — HAL ABI (contract first)

1. **`include/audio.h`** — format contract (mono s16le at
   AUDIO_RATE 16k), mixer channel ids (MEDIA/CALL_DL/CALL_UL/
   RINGER), backend registry {name,priority,caps,open/close/play/
   rec}, stream open/close/read/write APIs with played-frames
   counter, mixer_set/get_volume + mixer_scale pure fn, wav_info +
   wav_parse, modem PCM hooks (audio_call_dl_write/ul_read),
   ringtone and notification APIs, audio_tick, null tap accessors.

Commit: "Add audio HAL ABI header (phase 13)".

### Batch A2 — HAL core + null backend + wav + ringtone + I2S scaffold

2. **`drivers/audio.c`** — `am` state (volumes, call_routed flag,
   played counter, lock); backend registry with
   priority-based playback/capture selection; stream alloc +
   open/close for both directions; audio_write applies
   mixer_scale in 256-frame chunks (vol==255 bypasses the copy);
   audio_read delegates to the backend; the modem PCM hooks:
   audio_call_dl_write taps the downlink into a 512-frame sink
   when the PCM path is active, audio_call_ul_read drains a ring
   filled by a 600 Hz square marker generator (QEMU loopback);
   audio_on_route(state, arg) implements the phase-12 seam --
   CALL_ACTIVE arms the PCM path and pre-fills uplink, CALL_
   INCOMING starts the ringtone, IDLE closes the PCM path and
   stops the ringer -- registered via call_audio_route_set() in
   audio_subsys_init(); audio_tick() delegates to ringtone_tick.
3. **`drivers/audio_null.c`** — null backend: play counts frames
   and keeps the last NULL_TAP(256) written samples as the
   verification tap; rec synthesizes a fixed 1 kHz square from a
   phase counter; open/close guard single-instance per direction;
   audio_null_tap()/audio_null_play_frames() exported for tests.
4. **`drivers/wav.c`** — RIFF/WAVE walk (fmt + data chunks,
   word-aligned), validates PCM/16-bit/non-zero rate, clamps
   data_bytes to the buffer; wav_info{channels,rate,bits,
   data_off,data_bytes}.
5. **`drivers/ringtone.c`** — 125 ms melody steps alternating
   800/1000 Hz squares with gap half-cadence, generated 320-frame
   (20 ms) blocks into MIX_CH_RINGER via the HAL; start/stop/
   playing/tick per the header contract.
6. **`drivers/audio_i2s.c`** — I2S/DMIC scaffold: ops stubs return
   failure until configured; registration function provided for
   board bring-up (phase12-buttons pattern).

Sweep fixes: mixer registry took a const backend pointer (the
registry links it -- made mutable like chardev/i2c), the route
hook signature gained the void* arg to match call_audio_route_fn,
and a batch of editor-escaped kprintf strings was repaired by a
line-rewrite pass.

Commit: "Add audio HAL core with mixer, null backend, wav parser,
ringtone and I2S scaffold (phase 13)" + "Fix audio route-hook
signature and mixer registry mutability (phase 13)".

### Batch C — bring-up + milestone battery

7. **`kernel/phase13.c`** — registers the null backend and the I2S
    scaffold (order harmless: the scaffold refuses everything until
    configured), calls audio_subsys_init(plat) which arms the
    routing seam, spawns "audiotest" at priority 57.
8. **`kernel/selftest_audio.c`** — "audiotest": mixer_scale matrix
    (unity/half/negative/mute), playback with tap verification
    (unscaled at 255, scaled at 128), played-counter advance,
    in-memory WAV build + parse + play, then the VFS leg
    (/wav/test.wav write -> read -> parse -> play), capture tone,
    call route ACTIVE with dl-write/ul-read both directions and
    pcm-closed-on-idle, ringer auto-start on incoming + frames
    through the mixer + stop on idle. Summary "selftest: audio ok".
9. **`kernel/main.c`** — phase13_init() call site after phase12;
    housekeeping gains audio_tick(ms) (ringer synthesis +
    drain accounting) beside modem_tick; banner bumps to
    `phase 13`.
10. **`Makefile`** — `make test` passes phase 13 to the cumulative
    harness.

Commits: battery/wiring commit; audio.h include fix commit.

## Milestone mapping

- "play a WAV file" -> audiotest wav_tests: RIFF built, parsed,
  played through the mixer (tap-verified), and the same bytes
  round-tripped through /wav/test.wav on the VFS before playing --
  the file-backed play path the plan asks for, deterministic on
  ramfs and identical on mounted vfat/ext2.
- "call audio audible both directions" -> the seam consumer arms
  the PCM path on CALL_ACTIVE: downlink frames written through
  audio_call_dl_write land in the modem bus sink (tap-verified),
  uplink frames come back through audio_call_ul_read as the
  marker tone -- both directions exercised and asserted; on
  boards the same two hooks bind to the modem PCM bus config.

## Bugs found (and fixed) along the way

- **const backend in registry**: audio_backend_register took a
  const pointer but links it into the mutable registry list;
  signature made mutable (matches chardev/i2c registries).
- **Route-hook arity**: audio_on_route initially had no void*
  arg; call_audio_route_fn delivers one, fixed pre-commit.
- **Editor-escape corruption**: the selftest CHECK macro and
  several kprintf strings landed with literal backslash sequences
  from oversized pastes; repaired by line surgery and scripted
  rewrites, files committed only after green sweeps.

## Design decisions worth remembering

- **Tap-verified audio**: the null backend keeps the last written
  frames so headless tests assert amplitude/attenuation/routing by
  data, not by ear -- the same "verify by data" doctrine as the
  net tap and battery thresholds.
- **Volume as pure integer math**: ((s*vol+127)/255) is symmetric,
  deterministic and identical on the write path and in tests.
- **Call PCM lives in the seam consumer**: the modem layer owns
  nothing audio; audio.c owns the hooks and the routing state, so
  the phase-12 call state machine needed zero changes.
- **Ringer is a mixer client**: ringtone frames flow through
  MIX_CH_RINGER like any stream -- volume, routing and tap
  verification come free.

## Verification status

Per coordination decision no make/QEMU target ran this phase; sweep
over 8 new + 1 edited units:

```
CLEAN include/{audio.h,ringtone.h}
CLEAN drivers/{audio.c,audio_null.c,wav.c,audio_i2s.c,ringtone.c}
CLEAN kernel/{phase13.c,selftest_audio.c,main.c}
```

When integration lands expect, in order:

```
make test        # cumulative criteria incl. banner "phase 13"
serial adds:
  audio: HAL online (1 backends), call seam armed
  ...
  selftest: modem ok                        (phase-12 battery green)
  audiotest: phase 13 audio selftests
  audiotest: scale vol=255 unity / half / negative / mute
  audiotest: playback stream open / write / counter / tap lines
  audiotest: wav built / parse in-memory / played via HAL
  audiotest: wav file open / read back / parse from vfs / played
  audiotest: capture stream open / tone frames / non-silent
  modtest: call -> ...                      (phase-12 observer lines)
  audiotest: call active (seam) / downlink / uplink / marker
  audiotest: ringer auto-start / frames / stops
  selftest: audio ok

make run-display DISPLAYARGS="-display gtk"   # audio still null;
                                              # visible phases only
```

Notes carried forward:

- virtio-sound is documented-unavailable on the legacy transport
  (needs virtio 1.0); if the transport ever gains modern
  negotiation the backend registry takes a virtio-sound backend
  without HAL changes.
- Capture on QEMU is synthetic (1 kHz square); real DMIC capture
  is the I2S scaffold's HW bring-up item.
- Stereo files parse but downmix by right-channel drop; proper
  resampling/multi-channel belongs to the phase-15 polish bucket.
- No user-facing volume persistence; mixer state resets at boot.

