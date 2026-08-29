/*
 * perm.c - per-app permission table (phase 16, plan item 85).
 *
 * Static table, name-keyed, default-deny. Lookup is a linear scan
 * over a dozen entries -- the cost is irrelevant next to the
 * syscall it gates. The kernel never registers by pid: a respawned
 * app keeps its permissions because they follow the image name.
 */

#include <stddef.h>
#include <stdint.h>

#include "lib.h"
#include "perm.h"

struct perm_entry {
    const char *app;
    uint32_t    mask;
};

static const struct perm_entry perm_table[] = {
    /* the compositor composes and presents                         */
    { "compositor", PERM_UI_COMPOSE | PERM_FB_PRESENT },
    /* the protocol battery exercises both paths                    */
    { "uitest",     PERM_UI_COMPOSE | PERM_FB_PRESENT },
    /* telephony clients                                            */
    { "dialer",     PERM_MODEM },
    { "msgs",       PERM_MODEM },
    /* everyone else (init, sh, batteryd, contacts, clock, calc,
     * settings, hello, ...) falls off the end: default deny        */
};

uint32_t perm_lookup(const char *app)
{
    if (!app)
        return 0;
    for (unsigned i = 0; i < sizeof(perm_table) / sizeof(perm_table[0]);
         i++)
        if (strcmp(perm_table[i].app, app) == 0)
            return perm_table[i].mask;
    return 0;
}

bool perm_has(const char *app, uint32_t perm)
{
    return (perm_lookup(app) & perm) == perm && perm != 0;
}

void perm_dump(void)
{
    static const char *names[] = { "ui-compose", "modem", "fb-present" };

    kprintf("perm: %u entries, default deny\n",
            (unsigned)(sizeof(perm_table) / sizeof(perm_table[0])));
    for (unsigned i = 0; i < sizeof(perm_table) / sizeof(perm_table[0]);
         i++) {
        kprintf("perm: %-10s", perm_table[i].app);
        for (unsigned b = 0; b < 3; b++)
            if (perm_table[i].mask & (1u << b))
                kprintf(" %s", names[b]);
        kprintf("\n");
    }
}
