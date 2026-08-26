/*
 * chardev.c - character device registry.
 */

#include <stdint.h>
#include <stddef.h>

#include "chardev.h"
#include "lib.h"

static struct char_dev *devs[CHAR_DEV_MAX];
static unsigned ndevs;

static bool s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int char_dev_register(struct char_dev *cd)
{
    if (!cd || !cd->name || !cd->read || !cd->write)
        return -1;
    for (unsigned i = 0; i < ndevs; i++)
        if (devs[i] == cd)
            return 0;                   /* idempotent */
    if (ndevs >= CHAR_DEV_MAX)
        return -1;
    devs[ndevs++] = cd;
    cd->next = NULL;
    return 0;
}

struct char_dev *char_dev_find(const char *name)
{
    if (!name)
        return NULL;
    for (unsigned i = 0; i < ndevs; i++)
        if (s_eq(devs[i]->name, name))
            return devs[i];
    return NULL;
}

unsigned char_dev_count(void)
{
    return ndevs;
}
