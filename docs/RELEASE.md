# RELEASE.md — release process (phase 16, item 89)

## What a release is

A release is the output of `make release`: a directory
`build/release/` containing everything needed to boot, debug and
audit one exact build:

    build/release/
      kernel8.img      raw AArch64 boot image (what the loader reads)
      kernel.elf       unstripped ELF (symbols for post-mortems)
      manifest.txt     git hash, UTC date, sizes, sha256 per file
      docs/            RELEASE.md, FLASHING.md, BOARD_BRINGUP.md

The manifest pins the *source* (git hash) and the *bits* (sha256),
which is what makes the image reproducible: rebuilding the same
hash with the same toolchain reproduces the same sums.

## Build

    make all          # kernel.elf + kernel8.img (QEMU dev image)
    make release      # assembles build/release/ + manifest
    make test         # on-target battery: expect "selftest: release ok"

`BOARD=pinephone make release` selects the phone board in the
manifest; the kernel itself is board-agnostic until the platform
table gains a new entry (see BOARD_BRINGUP.md).

## Update flow (A/B)

Slots live on persistent storage; the slot table is one sector
with per-slot CRC, a monotonic `seq` version, boot-attempt and
confirmed flags (see include/abmgr.h):

1. The updater writes the NEW image into the inactive slot's
   window and seals it: `abmgr_slot_seal(other, lba, nsect, seq++)`.
2. `abmgr_switch()` flips the active bit; the device reboots into
   the new image.
3. Each unconfirmed boot increments `boot_attempts`
   (`abmgr_boot_begin()` runs early in boot).
4. When userspace is healthy it calls `abmgr_confirm()` — the
   phase-15 init is the natural owner; the release selftest
   exercises the whole flow against a ramdisk.
5. `abmgr_evaluate()` rolls back to the last confirmed slot once
   attempts hit AB_MAX_ATTEMPTS, bumping the rollback counter.

Rollback counters are monotonic: an image older than the current
`seq` can never become active again, which closes the downgrade
attack the counter exists for.

## Boot-time budget

kmain stamps `[perf] boot <ms>` right before the banner; the
<10 s goal is measured from the first timer tick to the banner.
The QEMU dev image boots in well under a second of kernel time;
board bring-ups report the same line over serial.

## Watchdog

`drivers/watchdog.c` is armed for the production image with a
10 s deadline (phase16.c). Housekeeping kicks every ~2 ms; the
timer IRQ checks the deadline and PSCI-resets on a stall. Boards
with an SP805/SBSA watchdog register the real device alongside
(see BOARD_BRINGUP.md); the software path remains as the logic
fallback.
