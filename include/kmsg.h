#ifndef KMSG_H
#define KMSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * kmsg.h - the kernel message ring (phase 16, plan item 87).
 *
 * Every byte kprintf emits also lands in a static byte ring, so
 * the boot log survives after the fact: the panic path dumps it
 * to /var/kmsg (flash-persisted on boards with a writable root),
 * and diagnostics can read it back without scrolling a serial
 * console. Locking is a tiny irq-safe spinlock -- kprintf runs
 * from tasks, IRQ tops and panic (interrupts off) alike; panic
 * writes are appended best-effort and can never block.
 */

/* called from printf.c's emit path; never blocks, IRQ-safe        */
void kmsg_putc(char c);

/* oldest-first line reader: returns false past the last line      */
bool kmsg_line(unsigned idx, char *out, unsigned cap);

/* number of complete lines currently in the ring                  */
unsigned kmsg_count(void);

/* write the whole ring to a VFS file (best-effort; 0/-errno)      */
int kmsg_dump(const char *path);

#endif /* KMSG_H */
