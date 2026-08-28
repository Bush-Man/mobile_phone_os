#ifndef RINGTONE_H
#define RINGTONE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Ringtone / notification path (phase 13, item 73): a procedural
 * two-tone melody synthesized onto MIX_CH_RINGER through the HAL.
 * ringtone_tick() is driven by audio_tick() (housekeeping) and
 * emits AUDIO_RATE/50 frames per call while playing.
 */

void ringtone_start(void);
void ringtone_stop(void);
bool ringtone_playing(void);
void ringtone_tick(uint64_t now_ms);

#endif /* RINGTONE_H */
