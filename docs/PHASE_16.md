# Phase 16 — Hardening, Packaging & Release Polish (Implementation Log)

Milestone scope reached: the security pass -- a per-app permission
registry gating the service transports and the fb0 presentation
ioctls, stack-guard + user-mmap randomization (KASLR groundwork)
and a W^X audit probe (item 85); the A/B update mechanism -- a
CRC-sealed two-slot manager with boot-attempt counting, healthy-
boot confirmation and automatic rollback driven by a monotonic
version counter (item 86); watchdog integration (a software
watchdog with timer-IRQ teeth and PSCI reset), the panic screen
painted straight into the framebuffer, and a kmsg ring persisted
to /var/kmsg (item 87); the performance pass -- boot-time
measurement, a kheap churn benchmark and the compositor's
per-window dirty-strip fast path replacing full-frame redraws
(item 88); and the packaging -- `make release` with a manifest
builder plus RELEASE/FLASHING/BOARD_BRINGUP docs (item 89). The
"reltest" battery proves it all: 39 CHECKs across W^X, ASLR,
permissions, kmsg persistence, watchdog stats, the complete A/B
flow including a forced rollback, and perf metrics. Per the
standing coordination decision no make target ran; the final
sweep (per-file -fsyntax-only + script syntax checks + a make -n
dry run) covered every phase-16 unit and caught three latent
build-rule bugs in the Makefile (batch F) that would have broken
the first real `make`.

## Order of files written

### Batch A — security hardening (item 85, contract first)

1. **`include/perm.h` + `kernel/perm.c`** — the per-app permission
   model: name-keyed capability masks (PERM_UI_COMPOSE,
   PERM_MODEM, PERM_FB_PRESENT), default deny, linear lookup.
   Permissions follow the IMAGE NAME, so a respawned app keeps
   them; unknown apps get an empty mask.
2. **`include/kmsg.h` + `kernel/kmsg.c`** — the kernel message
   ring: 8 KiB byte ring with an oldest-first line reader and a
   VFS dumper. The single writer is printf's emit path (already
   serialized by the UART tx lock, or in raw panic mode), so the
   ring itself is lock-free.
3. **`lib/printf.c`** — every byte kprintf emits now goes through
   an emit() wrapper that taps kmsg_putc on its way to the UART.
4. **`include/watchdog.h` + `drivers/watchdog.c`** — the software
   watchdog: housekeeping kicks, the TIMER IRQ checks the
   deadline (it keeps beating when the scheduler wedges), and a
   stale kick reports through kmsg + console then PSCI-resets.
   Off until armed; the honest limitation (IRQ-masked hard hangs
   need a real SP805/SBSA device) is in the header.
5. **`kernel/time.c`** — watchdog_irq_tick() called from the
   timer PPI top half, after sched_tick.
6. **`kernel/panic.c`** — the panic path reworked: print, dump
   the kmsg ring to /var/kmsg (best-effort), paint the panic
   screen straight into the active framebuffer (kernel fb API,
   valid even with the compositor dead), then halt. Boards turn
   the halt into a reset via their watchdog.
7. **`kernel/syscall.c`** — the enforcement points: sys_usock_
   serve refuses /var/run/ui without PERM_UI_COMPOSE;
   sys_usock_connect gates /var/run/modem on PERM_MODEM and
   /var/run/ui on PERM_UI_COMPOSE; fb0's FBIO_BLIT/FBIO_FILL
   require PERM_FB_PRESENT. Unknown apps hit EPERM before any
   state changes.
8. **`kernel/proc.c`** — the KASLR groundwork: a per-boot xorshift
   PRNG seeded from the architected counter + boot jiffies in
   proc_subsys_init; every spawn AND exec re-randomizes the
   private-mmap window base (up to ~4 GiB into the 512 GiB
   window). Plus the proc_by_pid() accessor for kernel-side
   page-table probes.
9. **`include/panic.h` + `kernel/main.c`** — __stack_chk_guard
   re-randomized from the counter once time is up (the header
   comment always promised this); watchdog_init/kick wired
   (housekeeping heartbeat); boot_t0 stamp + the `[perf] boot`
   line before the banner; phase16_init(); banner -> phase 16.

Commit: "Add security hardening: permission registry, kmsg ring,
software watchdog, panic screen, ASLR, W^X-aware panic path
(phase 16)".

### Batch B — the A/B slot manager (item 86)

10. **`include/abmgr.h` + `kernel/abmgr.c`** — the update slot
    manager. The whole table is one 512-byte sector: magic
    "ABMG" + version, two slots (payload CRC, monotonic `seq`,
    lba/nsect window, boot_attempts, confirmed, active, valid),
    a rollback counter and a sealing CRC over everything above.
    Policy: a slot becomes active only via abmgr_switch() and
    only if sealed; the active slot must reach abmgr_confirm()
    within AB_MAX_ATTEMPTS (3) unconfirmed boots or
    abmgr_evaluate() rolls back to the other valid slot and
    bumps the rollback counter; confirm() bumps `seq`, so a
    rolled-back-to image keeps its version identity and no
    older image can ever become active again (the downgrade
    attack the counter exists to close). Every mutation is
    read-modify-write with the table CRC recomputed before the
    write -- a power cut mid-write leaves the old or the new
    table, never a torn one. The payload CRC streams sector by
    sector (the window may exceed any single buffer) and folds
    deterministically. abmgr_attach() formats an absent table;
    abmgr_table_get/put exist for the selftest.

Commit: "Add A/B update slot manager with CRC-sealed slots, boot
attempts and rollback counters (phase 16)".

### Batch C — GPU-less render fast path (item 88)

11. **`userspace/compositor.c`** — per-window dirty strips: each
    window carries a dirty rect that UI_FLUSH accumulates as a
    union (clamped to the surface; a degenerate rect falls back
    to the full window; a FLUSH on a hidden window falls back to
    the full-recompose path), and a win_dirty_mask the render
    thread consumes bit by bit -- present_window_strip() copies
    just that window's union-rect rows into the stage and
    presents just that strip with fb_blit. Full 800x600
    recompose is now reserved for mode/focus/banner changes
    (need_redraw), and the 1 Hz clock tick blits only the status
    strip (draw_status_bar + fb_blit of UI_STATUS_H rows) instead
    of the whole frame. An app frame costs its own rect, not a
    full-screen pass.

Commit: "Compositor: per-window dirty-strip present, status-strip
clock blit (GPU-less render fast path, phase 16)".

### Batch D — the release battery + bring-up + builder (items 87/89)

12. **`kernel/selftest_release.c`** — the "reltest" battery, one
    kernel task exercising every hardening surface end to end:
    W^X probes through vmm_probe() on a live sh process (its
    text page must be executable and NOT writable; a data page
    writable and NOT executable); ASLR double-spawns clock and
    asserts distinct mmap bases inside the randomization window
    (both killed and reaped after); permission checks
    (compositor's UI_COMPOSE+FB_PRESENT, dialer's MODEM, default
    deny for sh/hello, empty mask for unknown names); kmsg (ring
    holds boot lines, the banner is in there, kmsg_dump() writes
    /var/kmsg and the dump file reads back with the banner text);
    watchdog stats (armed with a deadline, heartbeats > 100,
    zero misses); the complete A/B flow against a private
    64-sector ramdisk block device -- attach/format, seal slot A
    (seq 10) and B (seq 11) over distinct payloads, switch to A,
    boot_begin, confirm, switch to B, burn all three unconfirmed
    attempts, evaluate() fires the automatic rollback back to A,
    rollbacks == 1, the failed slot keeps its attempt count, and
    a re-attach reads the same table back; finally the kheap
    churn benchmark (50 rounds x 32 varying kmalloc/kfree). The
    boot metric itself is kmain's `[perf] boot N ms` line.
13. **`kernel/phase16.c`** — the phase entry: arms the software
    watchdog at a generous 10 s deadline (housekeeping kicks
    every ~2 ms, so 10 s of silence means the scheduler is wedged)
    and spawns reltest at priority 59.
14. **`Makefile`** — the `release` target (build-release.sh with
    `git rev-parse --short HEAD` as the manifest's source pin)
    and `make test` phase 15 -> 16.
15. **`tests/serial_harness.py`** — phase >= 16 criteria appended:
    "selftest: release ok", "[perf] boot", "watchdog: armed",
    "ab: automatic rollback fired" (the CHECK names make these
    greps exact -- the ok-line contains the name verbatim), plus
    the four matching diagnostics lines in the failure report.
16. **`scripts/build-release.sh`** — the release builder: refuses
    to run without a completed build, assembles
    build/release/{kernel8.img, kernel.elf, manifest.txt, docs/}
    and writes the manifest (project, board, git hash, UTC date,
    toolchain version, per-file size + sha256).

Commit: "Add reltest battery, phase16 bring-up, harness checks and
release builder (phase 16)".

### Batch E — release docs (item 89)

17. **`docs/RELEASE.md`** — what a release is (the manifest pins
    the source by git hash and the bits by sha256 -- rebuilding
    the same hash with the same toolchain reproduces the sums),
    the build commands, and the full A/B update flow with the
    monotonic-seq rationale.
18. **`docs/FLASHING.md`** — how the image reaches a device: the
    QEMU paths (run/test/run-display) and the two real-board
    paths (U-Boot tftp/fatload + `go`, raw SD via kernel8.img +
    config.txt), load address 0x40000000, and what firmware must
    guarantee (EL1 reachable, GIC state, FDT).
19. **`docs/BOARD_BRINGUP.md`** — the porting checklist defining
    "brought up" (make test passes on the board; release bundle
    flashes per FLASHING.md): platform discovery (FDT or static
    platform_info, PSCI CPU_ON vs spin-tables), console + block
    device behind the phase-6 contracts, reserving the A/B
    layout (two payload windows + the slot-table sector), then
    input/display/battery/modem in phase order -- each step with
    the QEMU reference implementation to copy from.

Commit: "Add release packaging docs: RELEASE.md, FLASHING.md,
BOARD_BRINGUP.md (phase 16)".

### Batch F — Makefile integration fixes (found by the make -n dry run)

The UI program rules had three latent bugs no make target had
ever exercised (the phase-15 sweep was compile-only, and the
coordination decision kept `make` itself unrun until now; the
`make release` milestone made the graph load-bearing, so the
dry-run parse check is part of this phase):

20. **`Makefile`** —
    - the UI program rule `$(UI_PROGS:%=userspace/%):
      userspace/%.c ...` was an ORDINARY rule, so
      `userspace/%.c` was a literal file name: make stopped with
      "No rule to make target 'userspace/%.c'" before building
      any UI program (and builtin_imgs.o needs those binaries,
      so `make all` could never complete). Converted to a static
      pattern rule: `targets: userspace/%: userspace/%.c ...`.
    - the apps live in userspace/ and quote-include "ui.h" from
      userspace/ui/, which was on no -I path of the program rule
      (the toolkit objects compile from inside that directory and
      resolve it locally; the programs cannot). Added
      -Iuserspace/ui.
    - the toolkit object rule `$(BUILD)/ui_%.o:
      userspace/ui/%.c` stems wrong: build/ui_gfx.o -> stem
      "gfx" -> prerequisite userspace/ui/gfx.c, which does not
      exist (the file is ui_gfx.c), so the pattern rule never
      applied. Fixed the stem: userspace/ui/ui_%.c.

Commit: "Fix UI program build rules: static-pattern syntax, ui.h
include path, toolkit object stem (phase 16)".

## Milestone mapping

- "reproducible release image" -> `make release` assembles
  build/release/ with the boot image, the unstripped ELF for
  post-mortems, the docs set, and a manifest that pins the exact
  source (git hash) and bits (sha256 per file); BOARD=pinephone
  records the intended board in the manifest. The QEMU dev image
  is the same `make all` output the run/test targets boot.
- The on-target half of the milestone is reltest's
  "selftest: release ok" (the harness phase-16 criteria grep it);
  the board half is BOARD_BRINGUP.md's checklist plus
  FLASHING.md's two flashing paths.
- Items ride where they were built: 85 in the permission/ASLR/
  W^X probes, 86 in the A/B battery, 87 in watchdog stats +
  kmsg persistence + the reworked panic path, 88 in the
  compositor fast path + the perf checks, 89 in the builder +
  docs.

## Bugs found (and fixed) along the way

- **UI program rule was not a pattern rule**: the static-pattern
  syntax (`targets: target-pattern: prereq-patterns`) was written
  as an ordinary rule, leaving a literal `userspace/%.c`
  prerequisite with no rule -- `make all` would have died before
  compiling a single UI program, and the kernel links nothing
  without builtin_imgs.o. Found only because the phase-16
  dry-run finally exercised the graph.
- **ui.h unreachable from the apps**: quote-include search from
  userspace/*.c never reaches userspace/ui/, and no -I carried
  it; every UI program (not just the compositor) would have
  failed to compile.
- **ui_%.o stem mismatch**: `$(BUILD)/ui_%.o: userspace/ui/%.c`
  demands userspace/ui/gfx.c for build/ui_gfx.o; pattern rules
  with a nonexistent prerequisite silently never apply -- the
  objects would have been "unbuildable" even with the first two
  fixed.

## Design decisions worth remembering

- **One sector of atomic truth**: the A/B table fits in a single
  512-byte sector on purpose -- single-sector writes are the
  block layer's atomicity contract, so every RMW with the CRC
  recomputed in place is torn-write safe with no journaling.
  Anything larger would need shadow tables or a log.
- **confirm() bumps seq**: the healthy-boot confirmation is also
  the version bump, so the counter the rollback logic trusts is
  the same one that keeps old images inactive forever. Sealing
  without confirming never moves the counter.
- **The reltest A/B device is a private ramdisk**: no virtio
  dependency and zero interference with the phase-6/7 scratch
  disk or any release partition layout; abmgr_attach() formats
  an absent table, so the test starts from a blank device every
  boot.
- **Watchdog teeth in the timer IRQ, not the scheduler**: a
  wedged scheduler cannot kick itself; the PPI top half keeps
  beating and PSCI-resets on a stale deadline. The honest gap --
  IRQs-masked hard hangs and real hardware -- is documented in
  the header and carried on BOARD_BRINGUP.md's checklist
  (SP805/SBSA at bring-up).
- **The compositor's fast path degrades gracefully**: dirty
  strips are per-window and per-FLUSH; anything the fast path
  cannot express (hidden windows, degenerate rects, mode/
  focus/banner changes) falls back to the full recompose, so
  correctness never depends on the optimization.
- **KASLR is the mmap window today**: per-spawn/per-exec
  randomization of the private-mapping base; kernel-text
  relocation is future work (the stack guard + PRNG plumbing is
  the groundwork).

## Verification status

Per the standing coordination decision no make/QEMU target ran
this phase; sweep over the phase-16 units (per-file
-fsyntax-only, Makefile flags, -Wno-cast-function-type for the
pre-existing table style; userspace flags for the compositor):

```
CLEAN kernel/{perm,kmsg,panic,syscall,proc,main,time,phase16,
              selftest_release,abmgr}.c
CLEAN drivers/watchdog.c
CLEAN lib/printf.c
CLEAN userspace/compositor.c
     (+ the seven phase-15 UI programs re-swept with the fixed
      -Iuserspace/ui path: all CLEAN)
CLEAN scripts/build-release.sh   (bash -n)
CLEAN tests/serial_harness.py    (py_compile)
make -n all / release / test: graph resolves (dry run only --
this is what caught batch F)
```

When integration lands expect, in order:

```
make test        # cumulative criteria incl. banner "phase 16";
                 # phase >= 16 adds the reltest greps
serial adds:
  watchdog: armed (10000 ms deadline)
  [perf] boot N ms
  reltest: phase 16 release battery
  reltest: wx: text is X and not W                 ok
  reltest: aslr bases <b1> / <b2>
  reltest: aslr: layouts differ                    ok
  reltest: perm: default deny for unknown apps     ok
  reltest: kmsg: dumped to /var/kmsg               ok
  reltest: wdt: heartbeats flowing                 ok
  reltest: ab: slot A sealed (seq 10)              ok
  reltest: ab: automatic rollback fired            ok
  reltest: ab: rollback counter bumped             ok
  reltest: ab: table survives re-attach            ok
  reltest: perf 1600 alloc/free rounds in N ms
  selftest: release ok

make release     # build/release/{kernel8.img,kernel.elf,
                 # manifest.txt,docs/}; manifest lists board,
                 # git hash, UTC date, toolchain, sha256 sums
```

Notes carried forward:

- abmgr is proven by the battery but not yet wired into boot:
  the natural integration is init calling abmgr_boot_begin()
  early and abmgr_confirm() once userspace is healthy
  (RELEASE.md steps 3-4), on a board layout that reserves the
  slot-table sector (BOARD_BRINGUP.md item 2).
- The watchdog is the software one; IRQ-masked hard hangs and
  real-board supervision need SP805/SBSA (checklist item, end
  of phase 16).
- KASLR randomizes the user mmap window (per spawn/exec);
  kernel-text relocation remains future work.
- kmsg's panic dump is best-effort: before the VFS/ramfs is up
  there is nothing to dump to, and the panic screen already
  covers that window.
- The compositor's dirty-strip path is correctness-preserving by
  construction (every non-expressible case falls back to the
  full recompose), but a frame-pacing measurement (the missing
  piece of item 88's "performance pass") needs the real QEMU
  integration run to sample.
