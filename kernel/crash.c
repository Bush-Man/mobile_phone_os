/*
 * crash.c - crash record persistence (phase 14, plan item 78).
 *
 * Deaths that indicate a bug (user faults, unhandled fatal signals)
 * append one line to /var/crash/records through the VFS. The write
 * happens on the dying task's own context: mark_zombie runs before
 * the address-space teardown, the process is still a schedulable
 * task, so a blocking write is legal. Only the file write may park;
 * there are no retry loops -- if the write fails, the RAM ring (the
 * phase-5 "[proc] FAULT" lines) still carries the record.
 *
 * Deliberately NOT dumped: the register frame and user memory. With
 * no debugger protocol in the OS yet the bytes would be write-only;
 * the actionable line (who died, how, exit code, when) is what both
 * the field report and `sh`'s crash command consume. A full core
 * file is a phase-16 hardening item once a reader exists.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "crash.h"
#include "lib.h"
#include "proc.h"
#include "time.h"
#include "vfs.h"

#define CRASH_RING_LINES  8u
#define CRASH_LINE_MAX    96u

#define CRASH_DIR     "/var"
#define CRASH_SUBDIR  "/var/crash"
#define CRASH_FILE    "/var/crash/records"

static char crash_ring[CRASH_RING_LINES][CRASH_LINE_MAX];
static unsigned crash_ring_head;        /* next write slot           */
static unsigned crash_ring_items;

static bool crash_paths_made;

static void ring_push(const char *line)
{
    kstrlcpy(crash_ring[crash_ring_head], line, CRASH_LINE_MAX);
    crash_ring_head = (crash_ring_head + 1) % CRASH_RING_LINES;
    if (crash_ring_items < CRASH_RING_LINES)
        crash_ring_items++;
}

void crash_init(void)
{
    crash_ring_head = 0;
    crash_ring_items = 0;
}

unsigned crash_ring_count(void)
{
    return crash_ring_items;
}

bool crash_ring_get(unsigned idx, char *line_out, unsigned cap)
{
    if (idx >= crash_ring_items || cap == 0)
        return false;

    /* ring reads walk oldest-first from (head - items + idx)       */
    unsigned slot = (crash_ring_head + CRASH_RING_LINES -
                     crash_ring_items + idx) % CRASH_RING_LINES;

    kstrlcpy(line_out, crash_ring[slot], cap);
    return true;
}

/* idempotent mkdir chain for /var/crash (ramfs mkdirs are cheap)   */
static void ensure_paths(void)
{
    if (crash_paths_made)
        return;
    vfs_mkdir(CRASH_DIR);
    vfs_mkdir(CRASH_SUBDIR);
    crash_paths_made = true;
}

/* tiny local formatter: kprintf's engine is UART-locked           */
static int append_str(char *dst, unsigned cap, unsigned pos,
                      const char *s)
{
    while (*s && pos + 1 < cap)
        dst[pos++] = *s++;
    return (int)pos;
}

static int append_u64(char *dst, unsigned cap, unsigned pos,
                      uint64_t v)
{
    char tmp[20];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < (int)sizeof(tmp));

    while (n-- && pos + 1 < cap)
        dst[pos++] = tmp[n];
    return (int)pos;
}

static int append_i64(char *dst, unsigned cap, unsigned pos, int64_t v)
{
    if (v < 0 && pos + 1 < cap)
        dst[pos++] = '-';
    return append_u64(dst, cap, pos,
                      v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v);
}

void crash_record(struct proc *p, int code, const char *why)
{
    char line[CRASH_LINE_MAX];
    unsigned pos = 0;
    struct file *f;

    if (!p)
        return;

    pos = (unsigned)append_str(line, sizeof(line), pos, "pid=");
    pos = (unsigned)append_u64(line, sizeof(line), pos,
                               (uint64_t)p->pid);
    pos = (unsigned)append_str(line, sizeof(line), pos, " name=");
    pos = (unsigned)append_str(line, sizeof(line), pos, p->name);
    pos = (unsigned)append_str(line, sizeof(line), pos, " why=");
    pos = (unsigned)append_str(line, sizeof(line), pos, why);
    pos = (unsigned)append_str(line, sizeof(line), pos, " code=");
    pos = (unsigned)append_i64(line, sizeof(line), pos, code);
    pos = (unsigned)append_str(line, sizeof(line), pos, " at=");
    pos = (unsigned)append_u64(line, sizeof(line), pos,
                               time_uptime_ms());
    if (pos + 1 < sizeof(line))
        line[pos++] = '\n';
    line[pos] = 0;
    ring_push(line);

    ensure_paths();

    /* O_APPEND keeps concurrent crash records from clobbering;
     * O_CREAT covers the first-ever record                       */
    if (vfs_open(CRASH_FILE, O_WRONLY | O_APPEND | O_CREAT, &f))
        return;                     /* RAM ring still has the line */
    f_write(f, line, pos);
    file_close(f);
}
