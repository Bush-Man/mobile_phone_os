#ifndef BUTTONS_H
#define BUTTONS_H

struct platform_info;

/*
 * GPIO buttons/keys (phase 9, plan item 52).
 *
 * Board-specific volume/power keys are described by a small per-board
 * pin table; boards without an entry (QEMU's `-M virt` wires PL061
 * controllers but no switches) simply register nothing and let the
 * virtio keyboard stand in for testing.
 */
void buttons_subsys_init(const struct platform_info *plat);

#endif /* BUTTONS_H */