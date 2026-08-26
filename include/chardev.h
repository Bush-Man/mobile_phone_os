#ifndef CHARDEV_H
#define CHARDEV_H

#include <stdint.h>

/*
 * Minimal character device registry (phase 6). Each device is a small
 * ops vector with a stable name; the future devfs (phase 7) will
 * expose these as filesystem nodes.
 */

struct char_dev {
    const char *name;
    void *priv;

    /* blocking read; returns bytes read, 0 = EOF-ish/none */
    int (*read)(struct char_dev *, char *dst, unsigned max);
    int (*write)(struct char_dev *, const char *src, unsigned n);
    unsigned (*poll)(struct char_dev *);        /* readable hint */

    struct char_dev *next;
};

#define CHAR_DEV_MAX 8

int  char_dev_register(struct char_dev *cd);
struct char_dev *char_dev_find(const char *name);
unsigned char_dev_count(void);

#endif /* CHARDEV_H */
