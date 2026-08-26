# Phase 3 — Interrupts & Timers (Implementation Log)

Milestone reached: GICv2-driven interrupts, ARM generic timer ticks,
jiffies/monotonic/wall timekeeping, SGI software interrupts and
tasklet bottom halves all pass their boot self-tests; periodic uptime
lines print once per second and UART RX input is echoed from an
interrupt-fed ring buffer. `make test` prints
`SMOKE TEST: PASS` (banner + uptime + echoed `phase3rx`).

## Order of files written

### Batch A — IRQ framework + GICv2 backend

1. **`include/irq.h`** — INTID ranges (`IRQ_SGI/PPI/SPI_BASE`,
   `NR_IRQS=288`, architected timer PPIs 26/27/29/30, spurious 1023);
   `irq_handler_t` top-half contract (bool return = claimed);
   `struct irq_stats {raised,handled,unhandled,spurious}`; DAIF
   intrinsics `irq_local_mask/unmask/save/restore`; API
   `irq_register/enable/disable/set_priority/set_trigger_edge/
   send_sgi/stats_get/dispatch`.
2. **`drivers/gic.h`** — backend interface (`gic_init`, per-line
   enable/disable/priority/trigger, `gic_send_sgi`, `gic_ack`
   returning a full IAR word, `gic_eoi`) so kernel/irq.c never touches
   controller registers directly.
3. **`drivers/gic.c`** — GICv2 distributor + CPU interface. Register
   map constants; `nr_enable_words()` from GICD_TYPER.ITLinesNumber;
   `set_byte_reg()` word-RMW helper for the byte-per-intid priority /
   target registers. Init sequence: quiesce CTLR → disable+unpending
   every line → route all SPIs to our interface mask (read back from
   banked ITARGETSR word 0) → uniform priority 0xa0 → level-sensitive
   ICFGR across PPI+SPI → IGROUPR zeros (**Group 0 only — see bug #1**)
   → CTLR Grp0 enable; CPU side PMR=0xff, CTLR Grp0. EOImode=0: one
   EOIR write does priority drop + deactivate. Version check: anything
   probed as non-v2 panics loudly ("GICv3 backend not brought up yet")
   rather than silently misbehaving — v3 deferred like phase-2's W^X.
4. **`kernel/irq.c`** — static handler table `[NR_IRQS]`, masked
   register/unregister, `irq_dispatch()` drain loop
   (ack → run_one → eoi until IAR id ≥ 1020), one-time unhandler
   warning, stats snapshot under DAIF save.
5. **`include/platform.h` / `kernel/platform.c`** — new fields
   `gicd_base/gicc_base/has_gic/gic_version`, `uart_irq`,
   `timer_irq`. New probes:
   - `probe_intc`: `/intc*` compatible string walk
     (`arm,cortex-a15-gic|arm,gic-400` → v2, `arm,gic-v3` → 3) and
     reg = two frames (GICD, GICC); local `compat_is()` +
     `prop_strlen()` helpers since lib has no str* routines.
   - `probe_serial`: also decodes the PL011 `interrupts` specifier
     (type 0 SPI +32 / type 1 PPI +16) — QEMU virt wires the console
     to SPI line 1 = **INTID 33** (not the 65 often assumed).
   - `probe_timer_irq`: reads `/timer*` interrupts triplet 3 (virtual
     timer PPI) and cross-checks against architectural constant 27.
6. **`kernel/exceptions.c`** — EXC_IRQ/EXC_FIQ now route into
   `irq_dispatch()` instead of falling through to the fault dump;
   sync/SError paths unchanged.

> Commit: `Add IRQ framework and GICv2 distributor/CPU-interface driver`

### Batch B — generic timer + timekeeping

7. **`include/time.h`** — `TIME_HZ 100`, init/read APIs,
   `time_uptime_ns/ms`, wall-clock set/get, tickless-ready
   `timer_arm_oneshot_ns` / `time_restart_periodic`.
8. **`kernel/time.c`** — virtual timer view (CNTV_*) chosen because it
   is always EL1-accessible without a hypervisor. CNTFRQ_EL0 probe
   (62.5 MHz on qemu virt); compare programmed as ABSOLUTE one-shot
   and re-armed inside the top half via read-back `+= period_ticks`
   (zero drift — observed tick spacing stays at 1.000 s over minutes);
   timer PPI configured edge-triggered at priority 0x00 so ticks are
   never delayed behind device lines. ns conversion splits secs/rem
   to stay inside u64 without __int128.

> Commit: `Add ARM generic timer driver with jiffies and monotonic/wall timekeeping`

### Batch C — deferred work queues

9. **`include/tasklet.h` / `kernel/tasklet.c`** — fixed 256-entry ring
   of `{fn,arg}`, no dynamic allocation so scheduling is legal from
   IRQ context; enqueue/pop sections guarded by DAIF saves; work
   functions run with the CALLER's masking state restored around them
   (**see bug #3 — the first version permanently masked IRQs**);
   stats `{queued,ran,dropped,peak_depth}` with one-time overflow
   warning.

Design note (plan item 20): "software interrupts" are GIC SGIs
(hardware-scheduled inter-CPU signals) — there is deliberately no
SMC/HVC/trap-based SWI path anywhere in the kernel; deferral is done
by tasklets.

> Commit: `Add deferred work queues (tasklets) drained outside IRQ context`

### Batch D — UART RX interrupts, self-tests, wiring

10. **`drivers/uart.c` (+ `.h`)** — RX path split:
    - top half `uart_rx_irq`: checks MIS for RXMIS|RTMIS (FIFO
      threshold + receive-timeout both needed — single chars would
      otherwise only fire the timeout line), drains DR until FR.RXFE,
      pushes bytes into a 128-byte ring under DAIF save, schedules
      the echo tasklet if anything moved;
    - bottom half `echo_tasklet`: pops the ring, echoes bytes (CR →
      CRLF so logs stay clean), runs in main-loop context so TX can
      never interleave with kprintf output mid-line;
    - `uart_rx_irq_init`: flushes any pre-arm FIFO content through
      the same path (**typed-ahead bytes are never lost — see bug #5**),
      registers INTID, priority 0x80 below the timer, unmasks
      IMSC.RXIM|RTIM after clearing stale ICR bits.
11. **`kernel/selftest_irq.c`** — `irq_time_selftest()`:
    - SGI 0 round trip: distributor → CPU interface → vectors →
      dispatch → top half sets flag (bounded busy-wait, panic on
      timeout);
    - chained SGI 1 → top half schedules tasklet → drained by the
      same context that sent it (proves the top/bottom split);
    - jiffies advance ≥ 3 ticks within a bounded spin;
    - monotonic ns strictly increases across a delay.
    Unmasks DAIF.I here — the first moment interrupts actually flow.
12. **`kernel/main.c`** final sequence: uart → el → vectors → platform
    (now printing rx INTID 33 / GICv2 bases / timer INTID 27) →
    relocation sanity → vmm_init → mem_selftest → gic_init →
    time_init → uart_rx_irq_init → irq_time_selftest → irq/tasklet
    stats print → banner `[OK] mobile_phone_os phase 3` → loop:
    `tasklet_drain()`, one `time: %llu.%03llu s uptime` line per HZ
    ticks, `wfi`.
13. **`tests/serial_harness.py` + `Makefile`** — smoke driver spawns
    QEMU with the console on a UNIX-socket chardev, captures serial
    into `build/serial.log`, sends `phase3rx\r` once the guest prints
    "echo armed" (retries every 2 s until echoed), PASS requires
    banner + uptime + echo before the deadline. Replaces the old
    `-serial file:` grep which could not exercise RX at all.

> Commit: `Complete phase 3: timer uptime ticks and interrupt-driven UART echo`

## Debugging saga (real bugs found, in discovery order)

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | every IAR returned INTID **1022** ("pending but not visible"); zero interrupts ever delivered | `IGROUPR` written all-ones: without security extensions QEMU refuses to signal Group-1 lines to EL1 | keep everything Group 0; enable GRP0 bits only |
| 2 | data abort EC=25, FAR odd address during early boot | `fdt_u32(reg + 1)` advanced ONE BYTE (GNU void\* arithmetic), misreading interrupt specifiers | cast to `(const uint32_t *)reg + 1` |
| 3 | system went silent forever right after the chain self-test | first `tasklet_drain()` ended with `irq_local_mask()` and never restored state — IRQs stayed masked for the rest of the boot | drain wraps work in save/restore of the caller's DAIF |
| 4 | dispatch treated 1020..1022 as real interrupts and EOI'd them | only 1023 was special-cased | treat ids ≥ 1020 as end-of-drain, never write them to EOIR |
| 5 | typed-ahead RX bytes lost depending on host timing | input raced `uart_rx_irq_init`; nothing re-evaluated the FIFO afterwards | init drains any pre-arm FIFO content through the normal echo path; harness waits for "echo armed" + retries |

Investigation techniques that paid off: host-side DTB probe dumping
exact `reg`/`interrupts` cells before writing the drivers; temporary
serial markers bracketing `irq_dispatch` entry/exit to prove eret
itself worked; raw register dumps (ISENABLER/ISPENDR/RPR/HPPIR/
CNTV_CTL/cntvct) printed from the boot path; counter instrumentation
on the RX pipeline. One measurement trap worth remembering: TCG boot
is slow enough that "the counter must have expired by now" assumptions
are wrong — guest-time ≠ wall-time; always derive timing from CNTVCT.

## Verification result

```
$ make test
SMOKE TEST: PASS

mobile_phone_os kernel
bringup: running at EL1 (boot EL1)
bringup: vectors installed at 0x40000800
platform: model "linux,dummy-virt"
platform: RAM 128 MiB @ 0x40000000
platform: console UART @ 0x9000000 (rx INTID 33)
platform: GICv2 dist 0x8000000 cpu 0x8010000 (timer INTID 27)
bringup: memory management up (TTBR0/TTBR1 + caches on)
selftest: pmm .............. ok
selftest: vmm .............. ok
selftest: kheap (400 allocs) . ok
mm: 30782/31963 frames free, heap 400/400 allocs
irq: GICv2 online
time: 62500000 Hz system counter, 100 Hz tick (INTID 27)
uart: interrupt-driven echo armed
selftest: irq (sgi, chain, timer) ok (raised 6 handled 6, tasklets ran 2)
[OK] mobile_phone_os phase 3
time: 1.xxx s uptime          <- one line per second thereafter,
...                              spacing drift-free (absolute re-arm)
phase3rx                      <- typed via socket chardev, echoed
                                 from tasklet context
```

Notes carried forward:

- GICv3 backend intentionally deferred; detection + loud refusal in
  place so a v3 machine fails fast instead of hanging (plan risk note:
  one internal API from day one — the `gic_*` seam already exists).
- Interrupts never nest yet; preemption on tick arrives with the
  phase-4 scheduler.
- `vmm_unmap` still leaks empty intermediate tables (phase-2 note).
- W^X enforcement still open (phase-2 note).
- Wall clock starts at epoch 0; `time_set_wallclock()` exists but no
  RTC source feeds it yet.

> Commit: `Add phase 3 implementation log`
