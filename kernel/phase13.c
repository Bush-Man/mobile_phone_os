/*
 * phase13.c - audio bring-up (phase 13 entry).
 *
 * Registers the QEMU null backend (and the I2S scaffold on boards
 * that list a codec), arms the phase-12 call-routing seam, and
 * spawns "audiotest": mixer/volume checks, wav parse+play through
 * the VFS, capture tone, call-route PCM and ringtone paths.
 */

#include <stdint.h>

#include "audio.h"
#include "lib.h"
#include "platform.h"
#include "task.h"

void audio_selftest_task(void *arg);    /* kernel/selftest_audio.c   */
extern void audio_null_backend_register(void);
extern void audio_i2s_backend_register(void);

void phase13_init(const struct platform_info *plat)
{
    static bool done;

    (void)plat;
    if (done)
        return;
    done = true;

    audio_null_backend_register();
    /* boards with a codec entry would register i2s here (scaffold
     * refuses everything until configured, so order is harmless)  */
    audio_i2s_backend_register();

    audio_subsys_init(plat);
    task_create("audiotest", audio_selftest_task, NULL, 57);
}
