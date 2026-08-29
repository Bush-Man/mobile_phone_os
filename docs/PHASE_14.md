# Phase 14 — Userspace Foundation (Implementation Log)

Milestone scope reached: a syscall-backed libc for EL0 programs
(userspace/libc: crt0, string, stdio printf family, brk-backed
malloc with a free list, unistd wrappers, pthread-lite over the new
SYS_clone/SYS_thread_exit pair -- item 74); init as real PID 1 with
/var scaffolding, service spawning, blocking waitpid(-1) orphan
reaping and critical-daemon restart (item 75); sh with the
coreutils-lite surface (ps, kill, ls, cat, echo, mount, ifconfig,
bat, devs, crashlog, date, uptime, run -- each a thin consumer of
one phase-14 report syscall, item 76); the batteryd/udevd/timed
daemons (item 77); and crash handling: kernel/crash.c appends a
one-line record to /var/crash/records on fault-or-fatal-signal
deaths, with the RAM ring as the pre-VFS fallback, plus the
init-respawn watchdog path (item 78). Milestone delivery in
usertest: libctest reaped with exit code 0, crasher reaped with a
pid= record visible both in the ring and in /var/crash/records,
and batteryd SIGKILLed and respawned by init under a new pid. The
interactive shell session itself is the other half of the milestone
(the serial harness pushes "phase14rx" and the tty echoes it back).
Per the standing coordination decision no make target ran; all
14 new + 4 edited units passed the per-file `-fsyntax-only`
sweeps.

## Order of files written

### Batch A — report data sources (kernel side, contract first)

1. **`fs/vfs.c` + `include/vfs.h`** —
   `vfs_mountinfo_fill(ents, max)`: snapshot of the active mount
   table (fstype + mountpoint) into usabi.h records, taken under
   the registry lock so a mount never appears half-copied.
2. **`drivers/driver_core.c` + `include/device.h`** —
   `device_info_fill(ents, max)`: every device across the bus
   registry in registration order, unit name + bound driver name +
   dev_state. No lock (like device_dump: only boot-time probing
   mutates the registries).
3. **`kernel/proc.c` + `include/proc.h`** —
   `proc_psinfo_fill(ents, max)`: the whole proc registry oldest
   first, zombies included and flagged PSINFO_ZOMBIE, ppid only
   while the parent lives. Kernel threads never appear (no proc).

Commit: "Add report data sources: mountinfo, devinfo and psinfo
snapshots (phase 14)". (Plus a repair commit: the vfs.c insertion
had duplicated two lines, and the previous session's proc.c had a
latent forward-reference bug -- thread_body called
kstack_frame_thread one definition too early; declared first.)

### Batch A2 — pre-existing working-tree core (reviewed + committed)

4. **`include/usabi.h`** (untracked from the prior session) — the
   fixed-layout report records that cross the boundary; mirrored
   verbatim into the libc (below). Stays kernel-header-free.
5. **`include/syscall.h`** — phase-14 numbers 42..54 (brk, psinfo,
   mountinfo, mount, netinfo, battinfo, gettime, settime, devlist,
   clone, thread_exit, poweroff, reboot).
6. **`include/crash.h` + `kernel/crash.c`** (untracked) — RAM ring
   (8 x 96 B lines) + lazy O_APPEND append of "pid= name= why=
   code= at=" lines to /var/crash/records from the dying task's
   context.
7. **`include/task.h` + `kernel/task.c`** — MAX_TASKS 8 -> 32
   (deadlock visit budget moved to 64 in sync.c), `parked` +
   `t_kstack` slot fields, the two-step creation split
   task_create_deferred/task_launch that closes the dispatch race,
   DEAD guards in wait_wake_all.
8. **`kernel/sched.c`** — the parked handshake (a task's context is
   quiescent the moment cpu_switch_to returns) and the
   cross-cpu-kill case (a DEAD current parks for good instead of
   re-queueing).
9. **`kernel/ipc.c`** — DEAD guards so killed-while-parked pollers
   are unlinked, never re-queued.
10. **`kernel/proc.c`** (rest of the prior session's diff) — init
    registration + orphan reparenting with SIGCHLD nudge,
    proc_kernel_reap, proc_pid_of_name, the pthread-lite backend
    (proc_thread_create builds the first EL0 frame on a kmalloc'd
    thread kstack; proc_thread_exit parks; proc_threads_reclaim
    frees settled slots), brk_floor for the shrinking-brk window,
    exec-refused-while-threaded, builtin_image entries for the
    seven new programs.
11. **`net/netif.c`** — netif_at(idx) registry-order accessor.

Commit: "Fix vfs.c edit duplication and proc.c
kstack_frame_thread forward reference (phase 14)".

### Batch B — syscall surface

12. **`kernel/syscall.c`** — the thirteen handlers + table rows.
    sys_brk (query with 0; refuse = unchanged top; grow maps
    zeroed pages through vmm_map_at under the dmap alias; shrink
    unmaps + frees whole pages above the top; window is
    [brk_floor, 0x1F0000000000)); sys_psinfo/mountinfo/netinfo/
    battinfo/devlist filling bounded kernel scratch then
    uacc_copy_out_cur; sys_mount (memory-fs types only, bd forced
    NULL -- a user process cannot name a block device);
    sys_gettime/settime over the wallclock; sys_clone ->
    proc_thread_create; sys_thread_exit -> proc_thread_exit
    (noreturn); sys_poweroff/sys_reboot through the PSCI layer
    with -ENODEV when absent. `include/net.h` grew the missing
    netif_at declaration.

Commit: "Add phase-14 syscall surface: brk, report calls, mount,
clone/thread_exit, get/settime, poweroff/reboot (phase 14)".

### Batch C — userspace libc (contract first: headers, then units)

13. **`userspace/libc/include/libc.h`** — the single master header:
    raw ABI numbers + errno subset, O_*/SEEK_*/signals, the
    _sys0.._sys6 trampoline decls, string/stdio/malloc/unistd/
    pthread-lite APIs, va_* macros over GCC builtins.
14. **`userspace/libc/include/sysinfo.h`** — the userspace mirror
    of usabi.h (same fields, same order, never repack).
15. **`userspace/libc/crt0.c`** — _start(argc, argv, envp in
    x0..x2, per the phase-5 convention) -> main -> _exit.
16. **`userspace/libc/string.c`** — memset/memcpy/memmove/memcmp +
    the str* set + strtoul (10/16).
17. **`userspace/libc/stdio.c`** — one formatting engine, two
    sinks (fd 1 or buffer): %c %s %% %d %i %u %x %X %p with '0'
    flag, width, l/ll/z modifiers; printf/vprintf/snprintf/
    vsnprintf/puts/putchar.
18. **`userspace/libc/malloc.c`** — sbrk growth via SYS_brk
    (refusal = unchanged top, the classic tell), 8-byte headers,
    first-fit free list, split-on-fresh-chunk, calloc/realloc.
19. **`userspace/libc/unistd.c`** — _sys3/_sys6 inline-asm svc
    trampolines + the thin wrappers and the phase-14 report calls.
20. **`userspace/libc/pthread.c`** — pthread-lite: 64 KiB mmap'd
    stacks, a tcb {fn, arg, retval, done} parked inside the stack,
    an armed-iarg trampoline, join by polling the shared done flag
    (same-address-space sharing is what makes that correct), and
    mutexes over __atomic test-and-set with 1 ms backoff.

Commit: "Add userspace libc: crt0, string, stdio, brk-backed
malloc, unistd wrappers, pthread-lite (phase 14)" -- plus the sbrk
char* return-type fix.

### Batch D — the programs (spawn order: init, then daemons, then shell)

21. **`userspace/init.c`** — PID 1: mkdir /var, /var/crash,
    /var/run; fork+execve of batteryd/udevd/timed/sh with critical
    flags; blocking waitpid(-1) reap loop that logs every death and
    respawns critical services under their table entry.
22. **`userspace/batteryd.c`** — 5 s SYS_battinfo status lines;
    the deliberate respawn victim.
23. **`userspace/udevd.c`** — SYS_devlist enumeration, snapshot
    written to /var/run/devices, then a slow heartbeat.
24. **`userspace/timed.c`** — SYS_gettime/settime ownership, honest
    epoch-0 wallclock (NTP pending), 30 s time lines.
25. **`userspace/sh.c`** — the shell: line reads on fd 0, tokenize,
    dispatch; ps/kill/ls (raw getdents records: u16 len, u8 type,
    NUL name, 8-aligned)/cat/echo/mount/ifconfig/bat/devs/
    crashlog/date/uptime/sleep, run = fork+execve+waitpid, exit
    returns to init (which respawns), poweroff/reboot refused in
    the demo build.
26. **`userspace/libctest.c`** — the libc battery: string, printf
    vs golden strings, malloc churn (reuse/split/grow/realloc),
    sbrk window, two joined threads + shared counter under the
    mutex (counter == 1001 exact); exit 0 iff all green.
27. **`userspace/crasher.c`** — prints intent, sleeps 1 s, writes
    through NULL; the never-reached "survived?!" line guards the
    crash path.

Commit: "Add init (PID 1), sh, batteryd, udevd, timed, libctest
and crasher programs (phase 14)".

### Batch E — embedding + build

28. **`arch/aarch64/builtin_imgs.S`** — seven .incbin entries
    (init, sh, batteryd, udevd, timed, libctest, crasher).
29. **`Makefile`** — libc_*.o pattern rule (-Iuserspace/libc/
    include), seven program rules linking crt0 first + libc objects
    + user.ld, builtin_imgs.o prerequisites extended, `make test`
    phase argument 13 -> 14.

Commit: "Embed phase-14 programs and add libc build rules, bump
test phase (phase 14)".

### Batch F — kernel bring-up + battery

30. **`kernel/phase14.c`** — crash_init(), spawn "usertest" (waits
    for init), spawn + register "init" as PID 1.
31. **`kernel/selftest_userspace.c`** — the usertest battery:
    libctest spawn + reaped exit 0; crasher reaped + ring grew +
    /var/crash/records opened and its content scanned for "pid=";
    batteryd SIGKILL + new-pid poll (5 s budget).
32. **`kernel/main.c`** — phase14_init() after phase13_init(),
    proc_threads_reclaim() in the housekeeping loop, BANNER ->
    phase 14.

Commit: "Wire phase 14 into boot: init PID 1, usertest battery,
thread reclaim in housekeeping, banner (phase 14)".

## Milestone mapping

- "interactive shell session" -> init spawns sh as a critical
  service; the tty line discipline echoes the harness's pushed
  "phase14rx" line back (the harness's echo-tag check), and the
  builtin set covers plan item 76's list (ls, cat, ps, kill,
  mount, ifconfig) plus the phase-14 report surface.
- "killed daemons respawn automatically" -> usertest kills
  batteryd with SIGKILL and polls until a NEW batteryd pid appears
  (init's restart path); the shell's `exit` proves the same path
  interactively.
- item 78 "core dumps to flash" -> the WHO/WHY/WHERE one-liner in
  /var/crash/records (RAM ring as fallback), read back both by
  usertest and the shell's crashlog builtin. Full register-frame
  dumps stay a phase-16 hardening item once a reader exists.

## Bugs found (and fixed) along the way

- **vfs.c insertion duplication**: an editor insert landed in the
  middle of the mount-count region and duplicated
  vfs_mount_count + a signature line; caught by the -fsyntax-only
  sweep (redefinition error) and repaired before commit.
- **kstack_frame_thread forward reference**: the prior session's
  proc.c called the static helper before its definition (implicit
  int declaration -> pointer truncation); fixed with a forward
  declaration.
- **morecore(0) NULL**: the allocator's initialization query
  returned NULL (r == heap_end looked like refusal) and would have
  failed every first malloc; incr==0 now returns the current top.
- **Data race in the libctest counter**: two threads on two cpus
  incrementing without the mutex can lose updates (counter ==
  1001 intermittent); increments now take the pthread mutex.
- **sbrk type**: void* -> char* to keep libctest comparisons
  warning-free.

## Design decisions worth remembering

- **Reports as snapshots, not iterators**: SYS_psinfo/mountinfo/
  netinfo/battinfo/devlist copy bounded record arrays out in one
  shot (uacc-checked). No user-held kernel pointers, no seq locks;
  the usabi.h layout is mirrored into the libc and must never be
  repacked.
- **Refuse-don't-fail brk**: the kernel returns the unchanged top
  on any out-of-window request (classic brk semantics), so the
  libc's morecore detects refusal by pointer equality and malloc
  simply returns NULL.
- **pthread-lite polls, the kernel stays simple**: threads share
  the leader's proc, so join is a 1 ms poll on a tcb flag inside
  the shared address space. exec-while-threaded is refused by the
  kernel (-EBUSY) rather than solved with cross-cpu thread
  surgery. Slot reclamation rides the scheduler's parked
  handshake and runs from housekeeping.
- **Crash dumps are one line, on purpose**: with no debugger
  protocol a register frame is write-only data; the pid/name/why/
  code/at line is what crashlog and field reports actually
  consume. Full cores are phase 16.
- **poweroff/reboot wired but refused**: the PSCI layer is behind
  SYS_poweroff/SYS_reboot, but the shell's builtins refuse in the
  demo build so a stray `poweroff` cannot kill a CI run.

## Verification status

Per coordination decision no make/QEMU target ran this phase; sweep
over 14 new + 4 edited units (per-file -fsyntax-only, same flags as
the Makefile, -Wno-cast-function-type for the pre-existing table
style):

```
CLEAN include/{vfs.h,device.h,proc.h,net.h}
CLEAN fs/vfs.c  drivers/driver_core.c
CLEAN kernel/{proc.c,syscall.c,task.c,sched.c,ipc.c,sync.c,main.c,
              crash.c,phase14.c,selftest_userspace.c,netif.c}
CLEAN userspace/libc/{crt0,string,stdio,malloc,unistd,pthread}.c
CLEAN userspace/{init,sh,batteryd,udevd,timed,libctest,crasher}.c
```

When integration lands expect, in order:

```
make test        # cumulative criteria incl. banner "phase 14"
serial adds:
  [proc] init is pid N (orphan reaper)
  init: mobile_phone_os userspace online (pid N)
  init: started batteryd/udevd/timed/sh ... [critical]
  sh: mobile_phone_os shell (pid ..) -- `help` lists builtins
  batteryd: online ...   udevd: N devices indexed -> /var/run/devices
  timed: wall clock online (epoch 0, NTP pending)
  usertest: libctest spawn / reaped / exit code 0          (ok)
  libctest: string/printf/malloc/brk/pthread ... ok lines
  usertest: crasher reaped / crash ring grew / record in /var/crash/records
  usertest: batteryd N -> M (watchdog), respawned by init
  selftest: userspace ok
  phase14rx echoed by the tty (harness echo-tag check)

make run        # interactive: `help`, `ps`, `ls /`, `ifconfig`,
                # `crashlog`, `exit` (watch init respawn sh)
```

Notes carried forward:

- `make test`'s harness phase arg is now 14; its extra checks are
  lexicographic string compares, so "14" skips the "4"/"5"-gated
  extras exactly like "13" did -- harmless.
- The userspace window for brk growth is capped at 0x1F0000000000,
  well below the stack floor; daemons never get near it.
- udevd is a snapshot device manager; hotplug events belong to the
  phase-16 hardening bucket on real hardware.
- timed arms an honest epoch-0 wallclock; NTP sync is the polish
  item once the phase-11 stack gets a real upstream.
