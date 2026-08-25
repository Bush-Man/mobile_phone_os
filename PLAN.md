# Mobile Phone OS — Development Plan

A 64-bit (AArch64) mobile phone operating system written in C, targeting ARM Cortex-A processors.

## Goals

- Bare-metal OS for ARMv8-A (Cortex-A53/A72/A76 class SoCs), 64-bit only (AArch64).
- Written almost entirely in C, with a minimal amount of AArch64 assembly where unavoidable
  (boot entry, context switch, syscall trampolines).
- Developed against QEMU (`qemu-system-aarch64 -M virt`) first; real hardware targets after bring-up.
- End state: a phone that boots to a touch UI with dialer/SMS/messaging apps over a cellular modem.

## Hardware Targets

| Stage | Target | Notes |
|-------|--------|-------|
| Dev   | QEMU `virt` machine | Emulated GICv2/v3, PL011 UART, virtio devices |
| HW-1  | Raspberry Pi 4/5 | Cortex-A72/A76, cheap, well-documented, no modem |
| HW-2  | PinePhone / similar SBC-phone | Allwinner A64 / RK3399-class, has modem, display, touchscreen |

## Repository Layout (target)

```
mobile_phone_os/
├── PLAN.md
├── Makefile
├── arch/aarch64/        # boot asm, MMU, GIC, timer, context switch, syscalls
├── kernel/              # scheduler, processes, syscalls, IPC, sync
├── mm/                  # page allocator, heap, page tables
├── drivers/             # uart, gic, mmc, framebuffer, gpio, i2c, usb...
├── lib/                 # string, printf, ringbuffer, list, kheap helpers
├── fs/                  # VFS, ramfs, fat32, ext2
├── net/                 # TCP/IP stack
├── ui/                  # compositor, widget toolkit
├── userspace/           # init, shell, libc, daemons, apps
├── include/             # shared headers
└── tests/               # host unit tests + on-target test suite
```

## Conventions

- C17, `-ffreestanding`, `-nostdlib`, no libc in kernel.
- Kernel is monolithic; drivers live in-kernel behind a registration API.
- Everything must run and be verifiable in QEMU before it touches real hardware.
- Every phase ends with a bootable image and a demo/test proving the feature works.

---

# Implementation Order

## Phase 0 — Toolchain & Build System
1. Install `aarch64-none-elf-gcc` cross toolchain + QEMU AArch64.
2. Makefile: kernel ELF → raw image, linker script at fixed load address.
3. Minimal `start.S`: park secondary cores, set up a temporary stack, jump into C.
4. Hello-world via UART0 (PL011) from C.
5. CI-style smoke test: `make run` boots in QEMU and prints banner.

**Milestone:** `[OK]` banner printed in QEMU.

## Phase 1 — CPU Bring-Up & Platform Discovery
6. Exception level management: detect current EL, drop from EL3/EL2 → EL1 as needed.
7. Set up vector table (VBAR_EL1) with full exception stubs; early panic handler.
8. Device Tree (FDT) parser: walk `/chosen`, `/memory`, UART nodes; store platform info structs.
9. MMU enablement: identity map kernel, map device regions (Device-nGnRE), enable caches.
10. Stack protector, `.bss` zeroing, relocation sanity checks.

**Milestone:** caches on, FDT memory size parsed and reported.

## Phase 2 — Memory Management
11. Physical frame allocator (buddy or free-list) driven by FDT memory ranges.
12. Page table API (4-level, 4 KiB granule): map/unmap/change perms.
13. Kernel heap: `kmalloc/kfree` (slab-ish free lists), `kzalloc`.
14. Higher-half kernel mapping; separate kernel text/data/device VA windows.
15. Guard pages + poison values in heap debug mode.

**Milestone:** kmalloc stress test passes under QEMU.

## Phase 3 — Interrupts & Timers
16. GIC driver (GICv2 for Pi/QEMU-virt-defaults, GICv3 path): distributor + CPU interface init.
17. IRQ/FIQ handling framework: register handlers by interrupt ID, top/bottom half split.
18. ARM Generic Timer: system counter read, per-CPU timer IRQ, tickless-ready design.
19. `jiffies` clocksource + monotonic/wall-clock timekeeping.
20. Software interrupts → SMC/HVC-free design; deferred work queues (tasklets).

**Milestone:** periodic timer ticks print uptime; UART RX interrupt echoes input.

## Phase 4 — Multitasking & Scheduling
21. Task struct, kernel stacks, context switch (callee-saved + SP/PC switch in asm).
22. Scheduler core: round-robin first, then priority + preemption on timer tick.
23. Idle thread per CPU; WFI-based idle.
24. SMP: bring secondary cores out of spin-table/PSCI parking, per-CPU areas, load balancing (basic).
25. Sleep/wakeup primitives: wait queues, `msleep`, `wake_up`.

**Milestone:** two kernel threads ping-pong printing with correct interleaving across CPUs.

## Phase 5 — Userspace, Syscalls & Processes
26. EL0/EL1 split: user page tables, user-mode entry/exit trampoline (exception vectors handle both ELs).
27. Process abstraction: address spaces (per-process ASID-tagged TTBR0), fork/exec-like creation.
28. Syscall table + `svc` dispatch path (fast, minimal asm).
29. ELF64 loader: PT_LOAD segments, .bss, argv/envp/auxv passing, dynamic-linker-ready layout.
30. Signals: delivery, handlers, default actions.
31. Copy-in/copy-out safety: validate user pointers, fault-tolerant accessors.

**Milestone:** static "hello" ELF binary runs at EL0, returns exit code to kernel.

## Phase 6 — Driver Framework & Core Drivers
32. Driver/bus/device model: match-by-compatible-string, probe/remove lifecycle, resource (MMIO/IRQ) allocation.
33. GPIO + pinctrl subsystem.
34. Serial console as a character device (tty layer with line discipline basics).
35. MMC/eMMC/SDHCI block driver + block layer (request queue, buffering, partitions: MBR/GPT).
36. Virtio-blk/virtio-net backends for QEMU development parity.
37. I2C/SPI controller drivers + generic device probing (needed by PMIC/touch later).

**Milestone:** read/write blocks to an SD image; partition table parsed.

## Phase 7 — Filesystems
38. VFS: mount tree, dentries/inodes-lite, file ops vectors, fd table per process.
39. `ramfs`/`tmpfs` (in-memory) as root during early boot.
40. FAT32 read/write (SD cards, EFI partition interop).
41. ext2/ext4-lite (no journaling replay initially) for the root filesystem.
42. `devfs`-style device nodes; mount namespace basics kept simple (single namespace).

**Milestone:** persist a file across reboot on SD/virtio disk; userspace can open/read/write/close.

## Phase 8 — IPC, Sync & POSIX-ish API
43. Mutexes, semaphores, spinlocks (with lock-debugging: owner tracking, deadlock detector).
44. Pipes, message queues, shared memory regions (refcounted, mappable).
45. Unix-domain sockets (local transport for later daemons/UI protocol).
46. Wait/poll multiplexing so userspace can wait on multiple fds.
47. Stabilize syscall surface: `open/read/write/ioctl/mmap/fork/exec/wait/exit/sleep/kill` set.

**Milestone:** two processes talk through a pipe and shared memory; shell can run children.

## Phase 9 — Graphics & Input
48. Display driver: framebuffer from firmware/DTB (simple-panel style); double-buffer + vsync when available.
49. Framebuffer as device node; software blitter (fill, copy, alpha blend, font rendering).
50. Input subsystem: event queue model (`EV_ABS/EV_KEY/EV_SYN`-like).
51. Touchscreen controller driver (I2C touch ICs on target HW; QEMU: USB tablet).
52. Buttons/keys driver (volume/power) + key repeater logic.

**Milestone:** draw UI test pattern; touch coordinates stream into an input reader process.

## Phase 10 — Power Management & Battery
53. PSCI interface: system reset/off, CPU hotplug, suspend entry.
54. CPU idle states: WFI → deeper states when safe; wake sources wired to GIC.
55. PMIC/I2C driver: battery voltage/current, fuel gauge reading, charger status.
56. Battery daemon policy: low-battery warnings, orderly shutdown thresholds.
57. Backlight control, screen timeout/suspend of display.

**Milestone:** battery percentage reported; system suspends on idle and wakes on touch.

## Phase 11 — Networking
58. Ethernet path first (virtio-net on QEMU): MAC driver → netif API.
59. TCP/IP stack (own compact IPv4/ICMP/UDP/TCP implementation, lwIP-inspired but self-written).
60. Sockets API for userspace (`socket/connect/bind/listen/recv/send/select`).
61. DHCP + DNS clients.
62. Wi-Fi driver integration (SDIO Wi-Fi chip on target HW), WPA supplicant daemon port.
63. TLS later (Phase 15 polish) — mark as stretch goal here.

**Milestone:** ping gateway from shell; fetch an HTTP page on hardware with Wi-Fi.

## Phase 12 — Telephony & Cellular
64. Modem abstraction: AT command engine over USB/UART with timeouts/retries.
65. SIM access (via modem), network registration status, signal strength reporting.
66. Voice call state machine: dial/answer/hangup, ring events, call audio routing hooks.
67. SMS: send/receive/decode (PDU mode), store in message DB (filesystem-backed).
68. Data connection (PPP or modem-native netif) feeding the Phase 11 stack.
69. USSD + cell broadcast (stretch).

**Milestone:** place a call and send/receive an SMS on target hardware.

## Phase 13 — Audio
70. Audio HAL: playback/capture streams, mixer, volume routing.
71. Codec driver (I2S/DMIC on target HW); QEMU fallback: null/virtio-sound.
72. Route call audio to modem (PCM bus config on target).
73. Ringtone/notification playback path.

**Milestone:** play a WAV file; call audio audible both directions.

## Phase 14 — Userspace Foundation
74. Port or write a small libc (syscalls-backed: stdio, string, malloc, pthread-lite).
75. `init` (PID 1): mount table setup, spawn services, reap orphans, restart critical daemons.
76. Shell + coreutils-lite (ls, cat, ps, kill, mount, ifconfig...).
77. Service daemons: udev-lite device manager, batteryd, timed (clock/NTP later).
78. Crash handling: core dumps to flash, watchdog auto-restart of crashed services.

**Milestone:** interactive shell session; killed daemons respawn automatically.

## Phase 15 — UI Framework & Phone Apps
79. Compositor daemon: owns framebuffer/input, window surfaces via shared memory + IPC protocol.
80. Widget toolkit in C: views, layouts, touch gestures, text shaping (basic), themes.
81. Lock screen (PIN optional), home screen, app launcher.
82. Apps: **Dialer**, **SMS/Messaging**, **Contacts** (fs-backed DB), **Settings** (wifi/bt/battery/display), **Clock**, **Calculator**.
83. Notifications framework + status bar (battery/signal/time icons).
84. On-screen keyboard component.

**Milestone:** end-to-end phone UX: unlock → dialer → call; receive SMS shows notification.

## Phase 16 — Hardening, Packaging & Release Polish
85. Security: W^X everywhere, KASLR, user/kernel pointer hardening audit, permission model per app.
86. OTA update mechanism: A/B partition scheme, bootloader rollback counters.
87. Watchdog integration, panic screens instead of silent hangs, kmsg ring persisted to flash.
88. Performance pass: scheduler tuning, GPU-less render optimization, boot-time reduction (<10 s goal).
89. Documentation, board bring-up guides, flashing instructions, release image builder.

**Milestone:** reproducible release image for one real phone board + QEMU dev image.

---

## Testing Strategy

- **Host unit tests:** allocator, VFS logic, parsers compiled for x86_64 host with stubs (`tests/`).
- **On-target test runner:** `make test` builds a kernel+initramfs test image; assertions print PASS/FAIL over serial; non-zero QEMU exit code on failure.
- **Hardware smoke checklist:** per-phase script (UART output grep, file persistence check, call test).

## Risk Notes

- Real-phone peripherals (modem, touch, PMIC) are vendor-specific; budget extra bring-up time in Phases 10–13.
- Keep every phase QEMU-verifiable so hardware issues never block kernel progress.
- GICv2 vs v3 differences handled behind one internal API from day one (Phase 3) to avoid rework.
