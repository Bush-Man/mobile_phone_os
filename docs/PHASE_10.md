# Phase 10 — Power Management & Battery (Implementation Log)

Milestone scope reached: a public PSCI layer over the platform-probed
conduit with VERSION/FEATURES queries plus SYSTEM_OFF/SYSTEM_RESET
entry points that deliberately never return (include/psci.h +
kernel/psci.c, item 53); an idle governor that took over the
scheduler's WFI branch with per-cpu accounting and a depth-policy
extension point, wake sources formally noted (timer, uart-rx) since
the GIC stays armed across WFI in this port -- structurally
satisfying "wake sources wired to GIC" (include/pm.h + drivers/pm.c
+ kernel/sched.c, item 54); an X-Powers AXP-family PMIC scaffold
over the phase-6 I2C registry with documented register map, unit
scalings and a voltage-LUT fuel-gauge approximation (drivers/
pmic_axp.c, item 55); a battery provider registry with pure
hysteresis policy (WARN 20% / CRITICAL 7%, +-2% recovery band), a
throttled 1 Hz sampling funnel driven from housekeeping -- zero
dedicated PM tasks -- and a /dev/battery snapshot node; on QEMU a
mock provider discharges 1%/6s so percentage reporting is
demonstrable headlessly and the shutdown funnel refuses mock data
(include/battery.h + drivers/battery.c, item 56); display-power
suspend/resume built on the phase-9 input activity stamp with the
identical pure-decision-function pattern, live blank after 30 s idle
and wake-on-touch (pm.c + input.c pulse, item 57). Milestone proof
runs in pmtest: "battery N% reported (discharging)" from the live
cache plus a real 50 ms blank/wake-on-touch cycle; selftest summary
"selftest: pm ok". Per the standing coordination decision no make
target was run; every phase-10 unit passed the usual per-file
`-fsyntax-only` sweeps.

## Order of files written

### Batch A — PSCI interface (item 53)

1. **`include/psci.h`** — contract first: SMC32 function-id
   constants (VERSION/CPU_SUSPEND/CPU_OFF/CPU_ON/SYSTEM_OFF/
   SYSTEM_RESET/FEATURES), PSCI_RET_* return codes, lifecycle
   (psci_init with the platform snapshot), raw psci_call(), queries
   psci_version()/psci_has_feature(), and the two noreturn system
   operations carrying an explicit hazard warning in their comment
   block.
2. **`kernel/psci.c`** — conduit caller duplicated deliberately from
   kernel/smp.c (documented cross-reference; smp_plat ownership
   stays there). version() returns 0 when the conduit is absent;
   FEATURES success==fn-exists; the destructive wrappers panic if
   PSCI is missing and panic again if the firmware dares return.

### Batch B — PM core: idle governor + display engine (items 54+57)

3. **`include/pm.h`** — pm_idle_depth enum (WFI today,
   DEEP_RESERVED placeholder), pm_cpu_idle() as the scheduler hook,
   wfi accounting, wake-source note table, and the display half:
   activity pulse input side, timeout accessor, enum
   pm_disp_action{STAY,SUSPEND_NOW,RESUME_NOW} with the PURE
   pm_display_decide(now,last,suspended,pending,timeout) signature,
   plus the runtime tick and state/counters.
4. **`drivers/pm.c`** — one subsystem spinlock; idle path increments
   per-cpu counters then executes wfi; wake table set-once. The
   display engine keeps last-activity + pending_resume; push-side
   (input.c) only stamps and raises pending -- the actual present()
   round-trip happens in housekeeping tick because gpu_cmd panics
   outside task context. Suspend fills the ACTIVE canvas black via
   fb_fill_rect and presents through fb_virtio_gpu_present(); resume
   restores control flow and logs, content repaint documented as
   client duty until a compositor exists.
5. Review fixes pre-commit: pm_init tried to assign SPINLOCK_INIT
   (macro expands to a brace-initializer, not an expression) --
   removed since .bss zeroing IS the initializer; the resume branch
   of decide() originally resumed unconditionally -- policy extended
   with a `pending` parameter so suspended-stays-dark and
   wake-on-touch both become deterministic test cases.

Commit: "Add PSCI system layer and power-management core ...".

### Batch C — battery stack (items 55+56)

6. **`include/battery.h`** — `struct battery_state`
   {present,voltage_mv,current_ma,percent,temp_deci_c,age_ms},
   provider interface {name,is_mock,read,next}, registry APIs,
   cached snapshot getter, charger hint, threshold knobs, pure
   battery_policy(prev,new) with the four-outcome enum
   (OK/WARN/EXIT_WARN/CRITICAL), mock debug surface
   (battery_mock_attached/force).
7. **`drivers/battery.c`** — registry + cached snapshot under one
   lock; policy function with a +-2% recovery band so a pack
   wobbling at a threshold cannot flap warnings; poll tick
   throttled to 1 Hz internally; "[LOW]" transitions logged; the
   shutdown funnel refuses mock data (psci_system_off reserved for
   REAL gauge readings); /dev/battery chardev read builds the
   snapshot line with local put_dec/put_str emitters (freestanding
   -- no snprintf); QEMU mock discharges 1%/6s from uptime and
   honours battery_mock_force() via a file-local `mock_forced_pct`
   latch so selftests stay deterministic.
8. **`drivers/pmic_axp.c`** — AXP209-style register map constants
   with unit scalings (BATV 12-bit @1.25 mV/LSB, discharge current
   0.5 mA/LSB, charge-status bit6, internal temp), probe walking
   the i2c registry x {0x34,0x36} via i2c_probe_addr + CHIP_ID
   read, voltage-LUT percent approximation explicitly marked
   EXPERIMENTAL. QEMU instantiates no I2C controller -> one skip
   line, provider never registered, mock takes over.
9. **`drivers/i2c_core.c` / `include/i2c.h`** — added
   `i2c_adapter_at(unsigned idx)` mirroring chardev's `char_dev_at`
   registry accessor; the one infrastructure gap the PMIC probe
   exposed (pure addition).

Commit: "Add battery stack: provider registry, policy with
hysteresis, /dev/battery node, QEMU mock and AXP I2C scaffold
(phase 10)".

### Batch D — wiring + selftest

10. **`kernel/sched.c`** — the scheduler's empty-run branch calls
    pm_cpu_idle(cpu) instead of the inline wfi (governor owns
    accounting + depth policy).
11. **`drivers/input.c`** — every input_push() stamps
    pm_input_activity(); this is the wake-on-touch path.
12. **`kernel/main.c`** — psci_init(&plat) right after EL drop
    (pointer captured early; fields filled by platform_self later --
    same pattern as smp_plat); wake-source notes for
    plat.timer_irq/uart_irq right after time_init; housekeeping
    gains pm_display_tick(ms) + battery_poll_tick(ms); phase10_init
    call site; banner bumps to `phase 10`.
13. **`kernel/phase10.c`** — boot-context entry: pmic_axp_probe()
    then battery_subsys_init(plat) (real-gauge-first, mock
    fallback), spawns "pmtest" at priority 55.
14. **`kernel/selftest_pm.c`** — "pmtest": PSCI conduit probes
    (VERSION + FEATURES for SYSTEM_OFF/RESET; destructive ops
    intentionally NOT invoked), idle accounting across real
    msleeps, the full display-policy matrix (STAY / timeout-SUSPEND
    / disable / dark-stay / RESUME-on-pending) plus a LIVE 50 ms
    blank + injected touch wake cycle with state/count assertions,
    battery-policy hysteresis table, mock force->snapshot->
    chardev-format flow, milestone line
    "battery N% reported (discharging)". Summary "selftest: pm ok".
15. **`Makefile`** — `make test` passes phase 10 to the cumulative
    harness (all earlier criteria still enforced).

Commits: PM-core commit (A+B), battery commit (C), wiring commit
(D items 10-15).

## Milestone mapping

- "battery percentage reported" -> battery[report] lines every 10 s
  from the mock curve (or real gauge on boards), the /dev/battery
  snapshot node, and pmtest's explicit
  "battery N% reported (discharging)".
- "system suspends on idle" -> display-power engine: black frame
  presented after the idle timeout, refreshes stop,
  "display: suspended after Nms idle".
- "wakes on touch" -> input_push stamps activity -> pending_resume
  -> next tick presents and logs "display: resumed on input";
  pmtest exercises that policy synthetically (matrix) and live
  (50 ms cycle).

## Bugs found (and fixed) along the way

- **SPINLOCK_INIT is not an expression**: pm_init assigned the macro
  to a lock field; removed in favour of .bss zeroing with a comment
  explaining that this IS the initializer semantics.
- **Unconditional resume**: the first decide() woke a suspended
  display every tick regardless of input; extended with the
  `pending` parameter so wake-on-touch is precisely testable.
- **Mock testability gap**: mock_read recomputed from uptime every
  sample, making forced percentages impossible; added the file-local
  `mock_forced_pct` latch (header-documented test surface).
- **char_dev_register const-qualification**: the registry links a
  mutable pointer; the battery node keeps a static mutable copy
  seeded from the const template.
- **Transient duplicate batt_read**: two implementations existed
  during assembly; the snprintf-based one was excised in favour of
  the bounded manual formatter.

## Design decisions worth remembering

- **Pure policy functions everywhere** (display decide, battery
  policy): runtime engines apply, selftests assert -- no test sleeps
  through 30-second timeouts and threshold tuning never touches
  control flow.
- **Housekeeping is the PM scheduler**: display tick + 1 Hz battery
  sampling ride the existing ~2 ms loop with internal throttling;
  MAX_TASKS untouched by this phase.
- **Mock never kills the machine**: the CRITICAL path checks
  provider->is_mock before psci_system_off(); CI data may warn but
  never power off the box.
- **PSCI destructive ops are real but unreferenced by tests**;
  FEATURES probing proves the conduit without ending the session.
- **Idle depth stays WFI on QEMU**: PM_IDLE_DEEP_RESERVED marks the
  extension point; per-board wake masks belong to HW bring-up data.

## Verification status

Per coordination decision no make/QEMU target ran this phase; sweep
over 9 new + 5 edited units:

```
CLEAN include/{psci.h,pm.h,battery.h,i2c.h}
CLEAN kernel/{psci.c,pm.c,battery.c,pmic_axp.c,phase10.c,selftest_pm.c}
CLEAN kernel/{sched.c,main.c}  drivers/input.c  drivers/i2c_core.c
(kernel/proc.c retains its single pre-existing %llx warning, HEAD-stable)
```

When integration lands expect, in order:

```
make test        # cumulative criteria incl. banner "phase 10"
serial adds:
  pmic-axp: no PMIC on any I2C adapter
  battery: no gauge found -- attaching mock-qemu provider
  pm10: gauge provider attached (mock-qemu)
  ...
  selftest: ipc ok  /  selftest: gfx ok       (earlier batteries green)
  pmtest: phase 10 power/battery selftests
  pmtest: psci conduit present / version query / features ...
  pmtest: wfi accounted in idle path
  display: suspended after XXXXms idle         (live 50 ms cycle)
  display: resumed on input
  pmtest: policy ... / hysteresis ... lines
  battery[report]: NN% ... lines               (1 Hz, mock curve)
  battery NN% reported (discharging)
  selftest: pm ok
  display: suspended after 30000ms idle        (live default, later)

make run-display DISPLAYARGS="-display gtk"    # watch blank/resume
```

Notes carried forward:

- CPU hotplug/suspend ENTRY points exist at the PSCI layer only;
  actually powering a secondary down belongs with real SMP power
  governance (phase-16 hardening window) -- documented, not stubbed.
- AXP percent is an OCV approximation; true coulomb counting waits
  for board bring-up data (phase-16), same bucket as button tables.
- Display resume does not repaint content (no compositor yet); the
  pattern owner (gfxtest) or a future shell triggers redraws.
- Mock battery publishes "discharging" (current_ma<0) only; a
  plugged-in variant belongs to the same phase-10 HW bucket.


