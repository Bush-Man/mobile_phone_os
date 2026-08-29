/*
 * timed.c - clock daemon (phase 14, plan item 77).
 *
 * Owns the wall clock: reads SYS_gettime, arms the wallclock base
 * from the monotonic counter if the epoch is still zero (the QEMU
 * dev image has no RTC; NTP sync is the phase-11-polish item), and
 * prints a time line every 30 s. All EL0 -- the settime/gettime
 * path gets its exercise from here and from the sh `date` builtin.
 */

#include "libc.h"

int main(int argc, char **argv)
{
    u64 epoch;

    (void)argc;
    (void)argv;

    epoch = gettime_ns();
    if (epoch == 0) {
        /* wallclock = adjustable offset + monotonic; arming the
         * offset at 0 keeps the clock honest (uptime-based) until
         * a real source exists                                   */
        settime_ns(0);
        printf("timed: wall clock online (epoch 0, NTP pending)\n");
    } else {
        printf("timed: wall clock online (epoch %llu ns)\n",
               (unsigned long long)epoch);
    }

    for (;;) {
        sleep_ms(30000);
        printf("timed: %llu s since epoch base\n",
               (unsigned long long)(gettime_ns() / 1000000000ull));
    }
    return 0;
}
