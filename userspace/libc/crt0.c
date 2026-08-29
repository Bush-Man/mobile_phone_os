/*
 * crt0.c - process entry (phase 14).
 *
 * The kernel builds the initial user stack: argc, argv[], envp[]
 * land exactly where the AAPCS64 entry expects them, so _start
 * receives them in x0/x1/x2 (same convention as the phase-5 hello).
 * crt0 turns that into main(argc, argv, envp) and exits with
 * main's return value -- a libc program never sees _start.
 */

#include "libc.h"

extern int main(int argc, char **argv, char **envp);

void __attribute__((noreturn))
_start(unsigned long argc, char **argv, char **envp)
{
    int code = main((int)argc, argv, envp);

    _exit(code);
    for (;;)
        ;
}
