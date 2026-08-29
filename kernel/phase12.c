/*
 * phase12.c - telephony bring-up (phase 12 entry).
 *
 * modem_subsys_init picks the transport (real "modem" chardev on
 * boards, scripted mock on QEMU); modtest then runs the milestone
 * battery: handshake, SIM/registration/signal queries, outbound and
 * inbound voice calls, SMS send + receive with PDU round-trip and
 * the /sms store. Data netif registered/unregistered as item-68
 * proof. Zero dedicated tasks: modem_tick rides housekeeping.
 */

#include <stdint.h>

#include "lib.h"
#include "modem.h"
#include "platform.h"
#include "task.h"

void modem_selftest_task(void *arg);    /* kernel/selftest_modem.c   */
void modemd_task(void *arg);            /* kernel/modemd.c           */

void phase12_init(const struct platform_info *plat)
{
    static bool done;

    (void)plat;
    if (done)
        return;
    done = true;

    modem_subsys_init(plat);
    if (!modem_present()) {
        kprintf("modem: no transport -- phase 12 idle\n");
        return;
    }
    task_create("modtest", modem_selftest_task, NULL, 56);
    /* phase 15: the telephony service broker -- serves the unix-
     * socket line protocol EL0 programs (dialer, msgs, the
     * compositor) consume instead of raw modem calls            */
    task_create("modemd", modemd_task, NULL, 40);
}
