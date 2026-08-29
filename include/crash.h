#ifndef CRASH_H
#define CRASH_H

#include <stdbool.h>
#include <stdint.h>

struct proc;

/*
 * Crash records (plan item 78): when a process dies of a fault or an
 * unhandled fatal signal, a compact one-line record is appended to
 * /var/crash/records on the VFS ("core dump to flash", scaled to
 * what a phone OS needs: the register frame would be unreadable
 * without a debugger, the WHO/WHY/WHERE line is what a field report
 * or a post-mortem session actually consumes).
 *
 * The file is opened, appended and closed in one shot from the dying
 * task's context (task context because the write may park on the
 * filesystem's buffer path). A full record ring is kept in RAM as
 * the fallback of last resort when the VFS is not up yet.
 */

void crash_init(void);        /* RAM ring only; VFS path is lazy    */

/* called from mark_zombie for "faulted"/"killed by signal" deaths */
void crash_record(struct proc *p, int code, const char *why);

/* RAM-ring readers (selftest + crash report tooling) */
unsigned crash_ring_count(void);
bool     crash_ring_get(unsigned idx, char *line_out, unsigned cap);

#endif /* CRASH_H */
