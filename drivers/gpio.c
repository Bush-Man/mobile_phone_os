/*
 * gpio.c - gpiolib core + pinctrl-lite registry.
 *
 * Global line space across controllers; request/owner bookkeeping;
 * per-line irq dispatch slots. Controllers implement the four data
 * ops; everything else is policy here.
 */

#include <stdint.h>
#include <stddef.h>

#include "gpio.h"
#include "lib.h"
#include "spinlock.h"

#define GPIO_CHIPS_MAX 4
#define GPIO_LINES_MAX 256
#define PINCTRL_MAX    4

static struct gpio_chip *chips[GPIO_CHIPS_MAX];
static unsigned nchips;

struct line_state {
    const char *owner;          /* NULL = free */
    gpio_irq_fn irq_fn;
    void *irq_arg;
};

static struct line_state lines[GPIO_LINES_MAX];
static spinlock_t gpio_lock = SPINLOCK_INIT;

static struct pinctrl_ctrl *pctrls[PINCTRL_MAX];
static unsigned npctrls;

/* ---- controller registry --------------------------------------------------- */

int gpio_chip_register(struct gpio_chip *chip)
{
    daif_state s;

    if (!chip || !chip->label || !chip->ngpio ||
        !chip->dir_out || !chip->dir_in || !chip->set || !chip->get)
        return -1;

    if (chip->base + chip->ngpio > GPIO_LINES_MAX)
        return -1;

    s = irq_local_save();
    if (nchips >= GPIO_CHIPS_MAX) {
        irq_local_restore(s);
        return -1;
    }
    chips[nchips++] = chip;
    irq_local_restore(s);

    kprintf("gpio: chip %s lines %u..%u\n",
            chip->label, chip->base, chip->base + chip->ngpio - 1);
    return 0;
}

unsigned gpio_chip_count(void)
{
    return nchips;
}

unsigned gpio_total_lines(void)
{
    return nchips ? chips[nchips - 1]->base +
                     chips[nchips - 1]->ngpio : 0;
}

static struct gpio_chip *find_chip(unsigned line, unsigned *off_out)
{
    for (unsigned i = 0; i < nchips; i++) {
        struct gpio_chip *c = chips[i];

        if (line >= c->base && line < c->base + c->ngpio) {
            if (off_out)
                *off_out = line - c->base;
            return c;
        }
    }
    return NULL;
}

/* ---- consumer API ------------------------------------------------------------ */

int gpio_request(unsigned line, const char *owner)
{
    daif_state s;
    int r = -1;

    if (!find_chip(line, NULL))
        return -1;

    s = irq_local_save();
    spin_lock(&gpio_lock);
    if (!lines[line].owner) {
        lines[line].owner = owner ? owner : "?";
        r = 0;
    }
    spin_unlock(&gpio_lock);
    irq_local_restore(s);
    return r;
}

void gpio_free(unsigned line)
{
    daif_state s;

    if (line >= GPIO_LINES_MAX)
        return;

    s = irq_local_save();
    spin_lock(&gpio_lock);
    lines[line].owner = NULL;
    lines[line].irq_fn = NULL;
    lines[line].irq_arg = NULL;
    spin_unlock(&gpio_lock);
    irq_local_restore(s);
}

const char *gpio_owner(unsigned line)
{
    return line < GPIO_LINES_MAX ? lines[line].owner : NULL;
}

int gpio_dir_out(unsigned line, bool val)
{
    unsigned off;
    struct gpio_chip *c = find_chip(line, &off);

    return c ? c->dir_out(c, off, val) : -1;
}

int gpio_dir_in(unsigned line)
{
    unsigned off;
    struct gpio_chip *c = find_chip(line, &off);

    return c ? c->dir_in(c, off) : -1;
}

void gpio_set(unsigned line, bool val)
{
    unsigned off;
    struct gpio_chip *c = find_chip(line, &off);

    if (c)
        c->set(c, off, val);
}

int gpio_get(unsigned line)
{
    unsigned off;
    struct gpio_chip *c = find_chip(line, &off);

    return c ? c->get(c, off) : -1;
}

/* ---- pin interrupts ------------------------------------------------------------- */

/*
 * Called from a chip's IRQ top half: dispatch every armed slot of
 * that chip. The top half has already cleared the source at the
 * controller before calling in.
 */
void gpio_irq_dispatch(struct gpio_chip *chip, unsigned mask)
{
    while (mask) {
        unsigned off = __builtin_ctz(mask);

        mask &= ~(1u << off);

        if (chip->base + off < GPIO_LINES_MAX &&
            lines[chip->base + off].irq_fn)
            lines[chip->base + off].irq_fn(chip->base + off,
                                           lines[chip->base + off].irq_arg);
    }
}

int gpio_irq_register(unsigned line, gpio_irq_fn fn, void *arg)
{
    unsigned off;
    struct gpio_chip *c = find_chip(line, &off);
    daif_state s;
    int r;

    if (!fn)
        return -1;

    /* ownership is mandatory: pin irqs belong to the requester */
    if (line >= GPIO_LINES_MAX || !lines[line].owner)
        return -1;

    if (c->irq_enable) {
        s = irq_local_save();
        r = c->irq_enable(c, off, true);
        irq_local_restore(s);
        if (r != 0)
            return -1;
    } else {
        return -1;                      /* chip cannot do pin irqs */
    }

    s = irq_local_save();
    lines[line].irq_fn = fn;
    lines[line].irq_arg = arg;
    irq_local_restore(s);
    return 0;
}

int gpio_irq_unregister(unsigned line)
{
    unsigned off;
    struct gpio_chip *c = find_chip(line, &off);
    daif_state s;

    if (line >= GPIO_LINES_MAX || !c)
        return -1;

    if (c->irq_enable)
        c->irq_enable(c, off, false);

    s = irq_local_save();
    lines[line].irq_fn = NULL;
    lines[line].irq_arg = NULL;
    irq_local_restore(s);
    return 0;
}

/* ---- pinctrl-lite ------------------------------------------------------------------ */

int pinctrl_register(struct pinctrl_ctrl *ctrl)
{
    if (!ctrl || !ctrl->ops || !ctrl->ops->mux_set)
        return -1;
    if (npctrls >= PINCTRL_MAX)
        return -1;

    pctrls[npctrls++] = ctrl;
    kprintf("pinctrl: %s (%u groups)\n", ctrl->label, ctrl->ngroups);
    return 0;
}

static bool name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int pinctrl_select(const char *group_name, unsigned func)
{
    for (unsigned i = 0; i < npctrls; i++) {
        struct pinctrl_ctrl *c = pctrls[i];

        for (unsigned g = 0; g < c->ngroups; g++) {
            const struct pin_group *grp = &c->groups[g];

            if (group_name && !grp->name[0])
                continue;
            if (group_name && !name_eq(grp->name, group_name))
                continue;

            for (unsigned p = 0; p < grp->npins; p++)
                if (c->ops->mux_set(c->priv, grp->pins[p], func) != 0)
                    return -1;
            return 0;
        }
    }
    return -1;
}

unsigned pinctrl_ctrl_count(void)
{
    return npctrls;
}
