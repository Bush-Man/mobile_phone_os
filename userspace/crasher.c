/*
 * crasher.c - deliberate crash generator (phase 14, plan item 78).
 *
 * Prints its intent, sleeps one second, then writes through a null
 * pointer. The user data abort funnels through proc_user_fault ->
 * mark_zombie -> crash_record, producing both the kernel report
 * line and the /var/crash/records entry the shell's `crashlog`
 * builtin reads. usertest asserts the record appeared.
 */

#include "libc.h"

int main(int argc, char **argv)
{
    volatile int *bomb = (volatile int *)0;

    (void)argc;
    (void)argv;

    printf("crasher: pid %d dereferencing NULL in 1 s\n", getpid());
    sleep_ms(1000);
    *bomb = 0xdeadbeef;         /* never reached                     */
    printf("crasher: survived?! (crash path broken)\n");
    return 3;
}
