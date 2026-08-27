#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

struct char_dev;

/*
 * Input subsystem (phase 9, plan items 50+52) -- an evdev-lite.
 *
 * Linux-flavoured numeric ABI so userspace can share headers with a
 * libc later; backends (virtio-input tablet/keyboard today, GPIO
 * buttons on phone boards, synthetic calibration feeds) all funnel
 * through input_push(). Readers get a fixed-record stream off
 * /dev/event0 -- one struct input_event per record, blocking reads,
 * EV_SYN records kept verbatim as batching markers exactly as the
 * plan's "EV_ABS/EV_KEY/EV_SYN-like" model implies.
 *
 * Key repeating (item 52): the repeat engine watches KEY_DOWN /
 * KEY_UP pairs and synthesizes value==2 repeats (the Linux autore-
 * peat convention) at a fixed cadence after an initial delay. The
 * buttons poller task drives it by calling input_tick_repeats().
 */

#define EV_SYN      0x00u
#define EV_KEY      0x01u
#define EV_ABS      0x03u

#define ABS_X       0x00u
#define ABS_Y       0x01u
#define BTN_TOUCH   0x14au              /* 330                        */

#define KEY_VOLUMEDOWN 114u
#define KEY_VOLUMEUP   115u
#define KEY_POWER      116u

/* repeat tuning (ms)                                              */
#define KEY_REPEAT_DELAY_MS 400u
#define KEY_REPEAT_RATE_MS   60u

/* wire record delivered through /dev/event0                       */
struct input_event {
    uint32_t ms;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed));              /* 12 bytes                   */

/* ---- subsystem ------------------------------------------------------------------ */

void input_subsys_init(void);
struct char_dev *input_event_dev(void);

/*
 * Append one event to the shared stream. Overflow drops are counted;
 * never blocks, safe from IRQ top halves.
 */
void input_push(uint16_t type, uint16_t code, int32_t value);

/* kernel-side convenience: read raw chardev bytes                  */
struct input_event_ring {
    uint64_t pushed;
    uint64_t dropped;
    uint64_t read_events;
};

void input_stats_get(struct input_event_ring *out);
unsigned input_pending(void);

/* periodic hook from the buttons/input poller task                 */
void input_tick_repeats(void);

#endif /* INPUT_H */