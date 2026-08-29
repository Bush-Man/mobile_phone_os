/*
 * panic.c - fatal error stop, stack-smashing guard, and the
 * phase-16 panic path: persist the kmsg ring, paint a panic
 * screen if a display is up, then halt (a boards' hardware
 * watchdog or the phase-16 software watchdog turns the halt
 * into a reset).
 */

#include <stdint.h>

#include "fb.h"
#include "kmsg.h"
#include "lib.h"
#include "panic.h"
#include "uart.h"

/* non-zero before any protected function runs; phase 16 randomizes
 * it from the system counter at boot (see kmain)                 */
uintptr_t __stack_chk_guard = 0x5f6cfa7d9e62b415ULL;

/* last-resort visual report: dark red field + the message. Uses
 * the kernel fb API directly -- valid even with the compositor
 * dead, and from any context fb_present() says is safe.          */
static void panic_screen(const char *msg)
{
    const struct fb_canvas *cv = fb_active();
    unsigned i;

    if (!cv || !cv->frames)
        return;

    fb_fill_rect((struct fb_canvas *)cv, 0, 0,
                 cv->width, cv->height, 0x004a1010u);
    fb_draw_text((struct fb_canvas *)cv, 24, 24,
                 "!! KERNEL PANIC !!", 0x00ffffffu, 0x004a1010u);
    fb_draw_text((struct fb_canvas *)cv, 24, 48, msg,
                 0x00ffb0b0u, 0x004a1010u);
    fb_draw_text((struct fb_canvas *)cv, 24, cv->height - 32,
                 "kmsg persisted to /var/kmsg",
                 0x00808080u, 0x004a1010u);

    for (i = 0; i < 2000000u; i++)       /* give the GPU a beat */
        __asm__ volatile("nop");
}

void panic(const char *msg)
{
    __asm__ volatile("msr daifset, #0xf");
    uart_panic_mode();
    kprintf("\nPANIC: %s\n", msg);

    /* best-effort: root may not be mounted; the ring itself is
     * always live for the post-reboot console reader            */
    kmsg_dump("/var/kmsg");

    panic_screen(msg);

    kprintf("System halted.\n");
    for (;;)
        __asm__ volatile("wfe");
}

void __stack_chk_fail(void)
{
    panic("stack smashing detected");
}
