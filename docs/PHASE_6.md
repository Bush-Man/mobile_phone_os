# Phase 6 — Driver Framework & Core Drivers (Implementation Log)

Milestone scope reached: a bus/device/driver model that enumerates the
flattened device tree into resource-carrying devices and binds drivers
by exact compatible-string match; gpiolib + pinctrl-lite with a PL061
controller driver; a tty line discipline that takes over console echo;
a block layer with sector cache, request queue, and MBR/GPT partition
parsing (CRC-validated); legacy virtio-mmio transport with virtio-blk
and virtio-net frontends; an SDHCI/MMC backend for real boards; I2C/SPI
cores with DesignWare-I2C and PL022 controller backends. Per the
standing coordination decision, final integration builds/QEMU smoke
runs are deferred to the project-complete point (phase 5 is being
developed in parallel in this tree); every file was kept compile-clean
via per-file `aarch64-linux-gnu-gcc -fsyntax-only` checks.

## Order of files written

### Batch A — driver/bus/device core

1. **`include/device.h`** — `struct resource {type: RES_MMIO/RES_IRQ,
   base, size, irq_type, irq_flags}` (intids precomputed SPI+32/PPI+16),
   `struct device` (name/compat pointers straight into the DTB blob,
   res[], bus/drv/state UNBOUND→BOUND→FAILED, priv, quiet_bind),
   `struct driver` (compat_table + probe/remove), `struct bus_type`,
   registry/lifecycle/enumeration API. Caps: 8 compats, 8 resources,
   64 devices.
2. **`drivers/driver_core.c`** — registries (`bus_register`,
   `driver_register`, `device_register`, idempotent), two-pass FDT
   walker: pass 1 harvests phandle → #interrupt-cells facts
   (`pass1`, order-proof via separate ph[]/ic[] tables); pass 2
   (`pass2`) creates devices for compatible nodes, parsing `reg`
   against inherited #address/#size-cells and `interrupts` against the
   interrupt-parent's icells. Skip list for non-device subtrees
   (memory/cpus/chosen/timer/intc/psci/pmu) plus a claimed-MMIO-base
   exclusion so the early-boot console UART is never re-probed.
   Matching (`driver_match`) = exact string compare across tables;
   `device_probe_one/all`; unmatched devices stay "nodrv", only
   matched-but-failed become FAILED.

> Commit: `Add driver/bus/device core with FDT enumeration (phase 6)`

### Batch B — GPIO + pinctrl

3. **`include/gpio.h`** — global line space over chips
   (`struct gpio_chip` ops: dir_out/dir_in/set/get + optional
   irq_enable), request/owner bookkeeping API, per-line irq slots,
   pinctrl-lite types (`pin_group`, `pinctrl_ops.mux_set/pull_set`,
   `pinctrl_select(group, func)`).
4. **`drivers/gpio.c`** — chip registry (max 4, lines ≤ 256),
   consumer API with owner tracking under one spinlock,
   `gpio_irq_dispatch(chip, mask)` fanned out from chip top halves,
   pinctrl registry walking groups by name.
5. **`drivers/pl061.c`** — QEMU-virt's real GPIO (0x09030000, SPI 7).
   Masked-addressing DATA accessors (address bits [9:2] select pins,
   register bits [8:1] carry data — atomic per-pin writes), DIR
   handling, rising-edge pin irqs armed through GPIOIS/IBE/IEV/IE/IC,
   top half reads MIS → acks via IC → dispatches gpiolib slots.
   Registered as `struct driver pl061_drv`.

> Commit: `Add GPIO/pinctrl subsystem with PL061 driver (phase 6)`

### Batch C — char devices + tty

6. **`include/chardev.h` / `drivers/chardev.c`** — tiny named ops-vector
   registry (read/write/poll) that devfs will front in phase 7.
7. **`include/tty.h` / `drivers/tty.c`** — canonical line discipline:
   editable current line (DEL/BS erase echoes `\b \b`, ^U kill, ^C
   flush, ^D EOF delivers partials, bell on overflow), completed-line
   fifo (8 × 128 B), blocking readers via phase-4
   `wait_sleep_when(...)` on queue-empty, raw mode passthrough,
   stats. RX path: uart ring → `tty_rx_tasklet` → `tty_rx_byte`.
   Registers itself as char dev `"console"`. Selftest injection hook
   `tty_test_feed()` bypasses hardware deterministically.
8. **`drivers/uart.{c,h}` additions (minimal, additive)** —
   `uart_echo_set(bool)` gates the raw echo tasklet (disabled tasklet
   *leaves bytes in the ring* for the tty instead of draining them)
   and `uart_rx_notify(fn)` routes rx-top-half notifications to the
   tty once attached; two drain passes around arming close the race
   window in `tty_init()`.

> Commit: `Add tty line discipline and char device registry (phase 6)`

### Batch D — block layer

9. **`include/block.h`** — `struct block_device` (512-byte logical
   sectors, capacity, max_sectors chunking hint, read/write ops,
   partition table storage), `struct blk_request`, buffered
   `block_read/block_write(bytes-based)`, MBR/GPT discovery API.
10. **`drivers/block.c`** — LE disk readers rd16/32/64; streaming
    CRC32 (poly 0xEDB88320); registry (≤ 4); direct-mapped write-through
    sector cache (16 lines/device; single-sector reads hit cache,
    multi-sector runs bypass it so streaming doesn't evict everything);
    `block_submit` chunks requests by driver max_sectors; partition
    code: MBR signature check, four-entry tables at 446, extended
    chains (0x05/0x0F/0x85, loop-guarded EBR walk), GPT detection via
    0xEE protective entry, header CRC validation ("EFI PART", field
    zeroed during checksum), single-pass entry-array CRC streaming +
    harvesting (esize divides sectors so entries never straddle),
    UTF-16LE label decode.

> Commit: `Add block layer with sector cache and MBR/GPT partition parsing (phase 6)`

### Batch E — virtio

11. **`include/virtio.h`** — legacy (version==1) mmio register offsets
    (HOST_FEATURES 0x010 … QUEUE_PFN 0x040 … STATUS 0x060, config at
    0x100), status bits, split-vring structs sized VQ_NUM=128,
    `struct virtio_dev` with per-device `complete()` callback,
    frontend attach prototypes.
12. **`drivers/virtio_mmio.c`** — transport driver matching
    `"virtio,mmio"`: empty sockets (DeviceID 0) bind quietly
    (`quiet_bind` keeps boot logs clean); version != 1 refused loudly
    (same stance as GICv3). DMA arena: static 128 KiB page-aligned
    bump pool inside the kernel image (physically contiguous by
    construction, which the PFN register requires) accessed ONLY
    through the uncached device-window alias (`virtio_dmap`,
    `virtio_unalias`) — zero cache maintenance, the same trick SMP
    bring-up uses. `virtq_setup` programs pagesize/num/align/pfn and
    threads the descriptor free list; `virtq_pop_desc/submit/
    free_chain/drain_used`; ISR acks InterruptStatus and schedules a
    bottom half that drains both queues. Frontend dispatch by devid:
    2 → virtio_blk_attach, 1 → virtio_net_attach.
13. **`drivers/virtio_blk.c`** — three-desc chains
    (16-B header {type,resv,sector} / staged data / status byte),
    uncached staging buffers copied in/out of caller memory,
    completion sets done/ok from the bottom half, blocking wrapper
    polls with msleep() and a 3-second jiffies deadline (no hang if a
    backend vanishes), registers as block device `"vblk0"`
    (capacity from config+0, max_sectors=8).
14. **`drivers/virtio_net.c`** — rx queue pre-posted with 8 reusable
    [10-B net header][1514-frame] slot chains (slots marked FREE on
    completion, re-armed by `virtio_net_poll()`/refill without leaking
    arena memory), tx single-frame send with CS-style staging copy and
    1-s timeout, MAC read from config, rx handler registry for the
    phase-11 stack, presence/MAC/send API for selftests.

> Commit: `Add virtio-mmio transport with virtio-blk and virtio-net frontends (phase 6)`

### Batch F — SDHCI/MMC (real-hardware targets)

15. **`include/mmc.h` / `drivers/sdhci.c`** — standard SDHCI register
    map at native byte/word widths; reset/power/timeout bring-up;
    clock tree (divider = ceil(base/(2·target)), even, 10-bit field,
    INT_CLK stable handshake); command engine polling CMD_COMPLETE /
    ERROR bits; R1/R136 response capture; classic card flow CMD0 →
    CMD8(0x1AA, v2 detect) → ACMD41(HCS loop) → CMD2 → CMD3(RCA) →
    CMD9(CSD geometry: v2 C_SIZE×1024 sectors, v1 fallback) → CMD7 →
    ACMD6(4-bit) → CMD16(512); PIO single-block reads (CMD17, BUF_RD
    windows) and writes (CMD24 then CMD13 until state leaves prg).
    Registered as block device with max_sectors=1 (correctness over
    throughput; the block layer hides it). Wired by board bring-up
    (`sdhci_register(base, name)`) since `-M virt` has no node.

> Commit: `Add SDHCI/MMC block driver for real hardware targets (phase 6)`

### Batch G — I2C/SPI

16. **`include/i2c.h` / `drivers/i2c_core.c`** — adapter registry,
    msg-based transfer wrapper, write/read/reg helpers with repeated
    start, quick-probe (reserved ranges excluded), generic probing:
    `i2c_enumerate_fdt_children()` walks direct children of a
    controller node (reg = 7-bit addr, compat carried) into client
    records.
17. **`drivers/i2c_dw.c`** — Synopsys DesignWare backend: disable/
    reconfigure cycles, std/fast SCL counts with IP minimums,
    TAR-programmed transactions, TX-abort (NACK) detection, stop-det
    cleanup, polling mode. `dw_i2c_register()` for board bring-up.
18. **`include/spi.h` / `drivers/spi_core.c`** — controller registry +
    `spi_sync`/`spi_write_then_read` managing active-low CS through
    gpiolib lines (PL022 has no native CS).
19. **`drivers/pl022_spi.c`** — PrimeCell SSP: CPSDVSR×(SCR+1) clock
    search minimizing error, CR0 DSS=8/mode bits/SCR, TNF/RNE polling
    full-duplex shift, BSY drain before CS release.

> Commit: `Add I2C/SPI subsystems with DW-I2C and PL022 controller drivers (phase 6)`

### Batch H — integration + selftest + demo targets

20. **`kernel/phase6.c`** — `phase6_init(&plat)` called from kmain
    right before the banner: subsys init → builtin driver registration
    (transport before frontends) → FDT enumeration (console UART
    excluded via claimed base) → probe-all → dump → `tty_init()`
    console takeover → spawns the **drvtest** task. Deliberate split:
    boot context never touches disks/tty reads because those park via
    msleep/wait queues (`msleep` dereferences `this_cpu()->current`,
    which is NULL off-task — caught in review before it could bite).
21. **`kernel/selftest_driver.c`** — drvtest body: gpiolib request/
    readback/duplicate-rejection checks against PL061; tty canonical
    line, DEL erase, ^U kill via deterministic injection; block stack
    pattern write/read/verify near disk tail + cached-read stability;
    blank-image detection → kernel writes its own MBR (type 0x83 @
    LBA 2048) → re-scan proves the parser round-trip (pre-existing
    tables are respected and merely rescanned); virtio-net MAC/TX
    report when a backend exists. Summary line
    `selftest: drivers ok` matches the harness grep style.
22. **`lib/string.c` + `include/lib.h`** — added the freestanding
    `memcmp` GCC is allowed to emit calls to (latent gap, now closed).
23. **`kernel/main.c`** — two-line footprint: extern decl + the marked
    phase-6 call site (kept minimal to avoid colliding with the
    parallel phase-5 stream editing this file).
24. **`Makefile`** — appended `disk.img` (64 MiB scratch, created
    once) and `run-disk` (virtio-blk-device + virtio-net-device with
    user networking) demo targets; no changes to existing targets.

> Commit: `Wire phase 6 driver bring-up, selftests and disk demo targets`

## Design decisions worth keeping

- **Compat strings point into the DTB blob.** `.rodata.fdt` lives
  forever in the image; devices hold pointers instead of copies.
- **Uncached aliasing instead of cache maintenance.** All virtio rings
  and staging buffers are kernel-image memory touched exclusively
  through `KERN_DEVICE_BASE` (Device-nGnRE). This dodges the flaky
  guest `dc cvac` behavior under MTTCG documented in phase 4 while
  keeping PFN-mandated physical contiguity trivially true.
- **Legacy-only virtio.** QEMU's default `force-legacy=true` layout
  (features 0x010, PFN queues, config at 0x100) is implemented;
  version 2 reads print a loud refusal, mirroring the GICv3 stance.
- **Console ownership handover.** Early boot keeps raw echo; tty_init
  flips `uart_echo_set(false)` (the disabled tasklet no longer drains)
  and arms `uart_rx_notify` — after which line discipline, erase, and
  canonical reads work. Phase 5's SYS_read should route through
  `tty_read`/the console char dev going forward; its direct
  `uart_rx_read` use predates the handover and will lose typed-ahead
  bytes once the tty owns the stream.
- **Blocking IO only from tasks.** Partition scans, block IO, and
  tty reads live in the drvtest task; bringup code that runs before
  schedulers start may only touch MMIO registers.
- **quiet_bind.** Empty virtio-mmio sockets bind silently and are
  omitted from the device dump, keeping enumeration evidence without
  drowning the log in 30+ identical lines.

## Verification status

Per coordination decision (parallel phase-5 development in this
tree), builds and the QEMU smoke run are deferred until the project-
complete point. Every new/edited translation unit was checked with
`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Wextra -ffreestanding
-mgeneral-regs-only` after each batch (zero warnings/errors at commit
time). When integration lands:

```
make run-disk     # boots with virtio-blk + virtio-net backends
                  # expect: drvcore enumeration/probe dump, tty banner,
                  #         drvtest PASS lines, selftest: drivers ok,
                  #         part: vblk0 holds 1 partition (type 0x83 @ 2048)
make test         # unchanged harness; expected-phase argv still 4/5-owned
```

Notes carried forward:

- Serial harness expected-phase number and the `[OK] ... phase N`
  banner remain owned by whichever stream integrates last.
- SDHCI/DW-I2C/PL022 are compile-verified only until real-board
  bring-up; wiring points are `sdhci_register()`, `dw_i2c_register()`,
  `pl022_register()`.
- GPT writing, ADMA2, multi-block transfers, and async request
  completions are deliberate non-goals for this phase.
- `remove()` paths exist but nothing unloads drivers at runtime yet.
