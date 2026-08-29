# FLASHING.md — how the image reaches a device

## QEMU dev image (default target)

No flashing: the kernel ELF runs directly.

    make run                    # serial console (-nographic)
    make test                   # headless battery, phase-16 criteria
    make run-display DISPLAYARGS="-display gtk"   # live UI

The scratch disk (`disk.img`, created on demand) holds the phase-6/7
filesystem experiments; the UI (phase 15) needs the virtio-gpu/
tablet/keyboard devices the run-display target already passes.

## Real boards

The raw image is loaded at **0x40000000** by whatever runs first
(the linker script fixes the address; kmain refuses to run
elsewhere). Two supported paths:

### 1. U-Boot (recommended for bring-up)

    # on the host: put the image on a TFTP server or an SD card
    tftp 0x40000000 kernel8.img      # or: fatload mmc 0:1 0x40000000 kernel8.img
    go 0x40000000

U-Boot must leave EL1 as the exception level, the GIC disabled and
the UART as configured by the FDT — see BOARD_BRINGUP.md for the
per-board checklist.

### 2. Raw SD card (Raspberry Pi 4/5 class)

    # kernel8.img into the FAT boot partition; config.txt with
    # arm_64bit=1, kernel=kernel8.img, enable_uart=1
    # the firmware loads it at 0x40000000 and starts cpu0 at EL2/EL1

The kernel drops from EL3/EL2 to EL1 on its own (phase 1); only
EL0-cold firmware that parks at EL3 without PSCI needs the
board-specific `plat` hooks filled in.

### 3. PinePhone-class (images from the release bundle)

    dd if=release/kernel8.img of=<mmc boot partition> bs=1k seek=512 conv=fsync

The A/B layout (RELEASE.md) expects two payload windows plus the
slot-table sector; the updater writes the new image into the
inactive window, never the one it is running from.

## After flashing

1. Serial console at the board's UART (PL011-style; the FDT tells
   the kernel which one).
2. Expect the boot banner `[OK] mobile_phone_os phase 16` and the
   boot-time line `[perf] boot <ms>`.
3. `/var/kmsg` holds the persisted kernel log of the previous run
   after a watchdog reset or panic.
