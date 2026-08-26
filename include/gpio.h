#ifndef GPIO_H
#define GPIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * gpiolib: global line numbers across all registered controllers.
 * A controller claims [base, base+ngpio); consumers use plain
 * unsigned line numbers. Lines are bookkeeping-only until requested,
 * mirroring the kernel-side half of a full gpio descriptor API.
 *
 * pinctrl-lite lives here too: controllers with pin muxing register
 * named groups; consumers select (group, function) pairs by name.
 */

#define GPIO_INVALID    ((unsigned)-1)
#define GPIO_OWNER_MAX  16

struct gpio_chip {
    const char *label;
    unsigned base;              /* global number of chip-local line 0 */
    unsigned ngpio;
    void *priv;

    int  (*dir_out)(struct gpio_chip *, unsigned off, bool val);
    int  (*dir_in)(struct gpio_chip *, unsigned off);
    void (*set)(struct gpio_chip *, unsigned off, bool val);
    int  (*get)(struct gpio_chip *, unsigned off);

    /* optional per-line interrupt support */
    int (*irq_enable)(struct gpio_chip *, unsigned off, bool on);

    struct gpio_chip *next;     /* registry linkage */
};

/* ---- controller registration --------------------------------------------- */

int gpio_chip_register(struct gpio_chip *chip);
unsigned gpio_chip_count(void);
unsigned gpio_total_lines(void);

/* ---- consumer API ---------------------------------------------------------- */

int  gpio_request(unsigned line, const char *owner);
void gpio_free(unsigned line);
const char *gpio_owner(unsigned line);

int  gpio_dir_out(unsigned line, bool val);
int  gpio_dir_in(unsigned line);
void gpio_set(unsigned line, bool val);
int  gpio_get(unsigned line);

/* ---- pin interrupts (controller-dependent) -------------------------- */

typedef void (*gpio_irq_fn)(unsigned line, void *arg);

/*
 * Registers fn for edge interrupts on `line` and arms the controller.
 * Returns 0 on success; -1 when the chip has no irq support or the
 * slot table is full. Dispatch happens from the chip's IRQ top half.
 */
int gpio_irq_register(unsigned line, gpio_irq_fn fn, void *arg);
int gpio_irq_unregister(unsigned line);

/* chip top halves call this with the latched mask to fan out */
void gpio_irq_dispatch(struct gpio_chip *chip, unsigned mask);

/* ---- pinctrl-lite ------------------------------------------------------------ */

#define PINCTRL_GROUP_MAX 8
#define PINCTRL_NAME_MAX  16

enum pin_pull {
    PIN_PULL_OFF = 0,
    PIN_PULL_DOWN,
    PIN_PULL_UP,
};

struct pin_group {
    char     name[PINCTRL_NAME_MAX];
    const unsigned *pins;
    unsigned npins;
};

struct pinctrl_ops {
    int (*mux_set)(void *priv, unsigned pin, unsigned func);
    int (*pull_set)(void *priv, unsigned pin, enum pin_pull pull);
};

struct pinctrl_ctrl {
    const char *label;
    void *priv;
    const struct pinctrl_ops *ops;

    struct pin_group groups[PINCTRL_GROUP_MAX];
    unsigned ngroups;

    struct pinctrl_ctrl *next;
};

int pinctrl_register(struct pinctrl_ctrl *ctrl);
int pinctrl_select(const char *group_name, unsigned func);
unsigned pinctrl_ctrl_count(void);

#endif /* GPIO_H */
