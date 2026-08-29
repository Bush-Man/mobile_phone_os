# BOARD_BRINGUP.md — porting checklist for a new phone board

Phase 16 (item 89) deliverable. A board is "brought up" when
`make test` passes on it and the release bundle flashes per
FLASHING.md. Work down the list; each step has a proven QEMU
reference implementation to copy from.

## 1. Platform discovery (phase 1 code)

- [ ] FDT / board file: RAM base+size, console UART (reg + IRQ),
      GIC distributor/CPU interface, timer IRQ. QEMU reference:
      `kernel/platform.c` (FDT walk) — boards without a usable FDT
      add a static `platform_info` instead.
- [ ] CPU release path: PSCI `CPU_ON` (QEMU reference:
      `kernel/psci.c`); SMCCC-less firmware needs the board's
      spin-table address filled into `smp.c`.

## 2. Console + storage

- [ ] UART driver: PL011 works as-is; 8250/custom submit a
      `char_dev` (phase 6 registry) with the same name the FDT
      points at.
- [ ] Block device: eMMC/SDHCI behind the phase-6 `block_device`
      contract (`drivers/virtio_blk.c` is the reference). The
      partition scan needs an MBR or GPT label.
- [ ] Reserve the A/B layout: two payload windows + the slot-table
      sector (include/abmgr.h; RELEASE.md has the update flow).

## 3. Peripherals (phase order preserved)

- [ ] Input: touch controller -> `input_push()` records
      (phase 9 contract); GPIO buttons map to KEY_POWER/VOLUME*.
- [ ] Display panel: submit an `fb_backend` whose claim() hands
      out the 4 KiB-page scatter list (phase 9; virtio_gpu.c is
      the reference for the backend contract).
- [ ] Battery/PMIC: `battery_provider` (phase 10) with a real
      gauge; the QEMU mock stays dev-image-only.
- [ ] Modem: AT transport over USB/UART (phase 12 contract);
      register it as the "modem" chardev and modemd picks it up
      unchanged.
- [ ] Audio codec: I2S backend (phase 13 scaffold awaits config).

## 4. Watchdog

- [ ] Real watchdog (SP805/SBSA): arm it at the end of phase16
      init with a deadline > the software watchdog's, kick it from
      the same housekeeping hook. The software watchdog stays
      enabled as the scheduler-level backstop.

## 5. Acceptance

- [ ] `make test` green over this board's serial (all phases).
- [ ] Panic path verified: force one, confirm the panic screen and
      /var/kmsg on the next boot.
- [ ] A/B update + forced rollback exercised on the device.
- [ ] Boot time `[perf] boot` under 10 s; record it in the release
      manifest notes.
