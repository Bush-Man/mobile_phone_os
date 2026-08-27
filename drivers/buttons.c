/*
 * buttons.c - board GPIO volume/power keys feeding the input core
 * (phase 9, plan item 52), with debounce and repeat support via the
 * shared input_tick_repeats() hook called from housekeeping.
 *
 * Pin tables are keyed on the platform model string from the FDT;
 * a missing entry means "this board has no wired keys" -- the probe
 * logs one line and exits cleanly so task slots stay available.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "buttons.h"
#include "gpio.h"
#include "input.h"
#include "lib.h"
#include "platform.h"
#include "task.h"

#define BTN_POLL_MS      20u
#define BTN_DEBOUNCE     2u              /* consecutive samples        */
#define BTN_PINS_MAX     4u

struct btn_pin {
    unsigned line;                      /* global gpio number         */
    uint16_t code;                      /* EV_KEY code                */
    bool     active_low;
};

struct btn_board {
    const char  *model_prefix;
    unsigned     active_low;            /* default for all pins       */
    struct btn_pin pins[BTN_PINS_MAX];
    unsigned     npins;
};

/* future phone boards fill this in (phase 10+ bring-up); QEMU's
 * "-M virt" intentionally matches nothing                           */
static const struct btn_board boards[] = {
    { .model_prefix = "raspberrypi", .active_low = true, .npins = 0 },
    { .model_prefix = "pine",        .active_low = true, .npins = 0 },
};

static struct {
    struct btn_pin pins[BTN_PINS_MAX];
    unsigned npins;

    struct {
        bool down;
        unsigned stable;                /* debounce counter           */
    } state[BTN_PINS_MAX];

    unsigned samples;
} bt;

static bool pin_pressed(const struct btn_pin *p)
{
    int v = gpio_get(p->line);

    if (v < 0)
        return false;
    return p->active_low ? !v : !!v;
}

/*
 * Poller task: samples the wired keys, debounces, feeds transitions
 * into the input core. Exits when nothing is configured -- on QEMU
 * the virtio keyboard provides key events instead.
 */
static void btn_poller(void *arg)
{
    (void)arg;

    for (;;) {
        for (unsigned i = 0; i < bt.npins; i++) {
            const struct btn_pin *p = &bt.pins[i];
            bool pressed = pin_pressed(p);

            if (pressed == bt.state[i].down) {
                bt.state[i].stable = 0;
                continue;
            }

            if (++bt.state[i].stable >= BTN_DEBOUNCE) {
                bt.state[i].down   = pressed;
                bt.state[i].stable = 0;
                input_push(EV_KEY, p->code, pressed ? 1 : 0);
            }
        }

        msleep(BTN_POLL_MS);
    }
}

void buttons_subsys_init(const struct platform_info *plat)
{
    const struct btn_board *board = NULL;

    for (unsigned i = 0; i < sizeof(boards) / sizeof(*boards); i++)
        if (plat->model[0] &&
            !strncmp(plat->model, boards[i].model_prefix,
                     strlen(boards[i].model_prefix))) {
            board = &boards[i];
            break;
        }

    if (!board || board->npins == 0) {
        kprintf("buttons: no GPIO keys mapped for \"%s\" "
                "(virtio keyboard covers input)\n",
                plat->model);
        return;
    }

    bt.npins = 0;
    for (unsigned i = 0; i < board->npins; i++) {
        if (gpio_request(board->pins[i].line, "buttons"))
            continue;
        gpio_dir_in(board->pins[i].line);
        bt.pins[bt.npins++] = board->pins[i];
    }

    if (!bt.npins) {
        kprintf("buttons: no lines claimable\n");
        return;
    }

    if (task_create("btnpoll", btn_poller, NULL, 60) < 0)
        kprintf("buttons: poller task failed\n");
}
