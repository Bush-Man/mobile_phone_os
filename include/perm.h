#ifndef PERM_H
#define PERM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * perm.h - the per-app permission model (phase 16, plan item 85).
 *
 * Every EL0 app has a capability mask, keyed by its process name
 * (the same name the builtin image table and psinfo carry). The
 * kernel consults it at the boundaries where a process can reach
 * something device-shaped or service-shaped:
 *
 *   PERM_UI_COMPOSE  connect("/var/run/ui")        (compositor protocol)
 *   PERM_MODEM       connect("/var/run/modem")     (dial/SMS control)
 *   PERM_FB_PRESENT  fb0 FBIO_BLIT/FBIO_FILL       (present frames)
 *
 * The enforcement points live in syscall.c (transport connect) and
 * the fb0 ioctl path. Unknown apps get an empty mask: the default
 * is deny, and the release selftest proves a shell cannot open the
 * modem transport.
 *
 * This is a phone, so the table is small and static; a signed-
 * manifest format belongs to the packaging work (docs/RELEASE.md).
 */

enum app_perm {
    PERM_UI_COMPOSE = 1u << 0,
    PERM_MODEM      = 1u << 1,
    PERM_FB_PRESENT = 1u << 2,
};

/* capability mask for `app` (0 for unknown -- default deny)        */
uint32_t perm_lookup(const char *app);

/* convenience: mask & perm != 0 */
bool perm_has(const char *app, uint32_t perm);

/* debug/diagnostic: list the table over the console                */
void perm_dump(void);

#endif /* PERM_H */
