# Phase 4 — Multitasking & Scheduling (Implementation Log)

Milestone reached: static-pool tasks with hand-crafted initial
contexts, an AArch64 callee-saved context switch, a priority
round-robin scheduler with tick-driven preemption, WFI idle built
into every cpu's scheduler loop, PSCI-based SMP bring-up (cpu1 runs
its own MMU/GIC/timer bring-up), wait queues with atomic
check-and-block semantics, and `msleep`. The milestone pair
("ping"/"pong") alternates through a strict-turn handshake and a
1000-round cross-cpu selftest asserts the exact 0,1,2,... sequence.

## Order of files written

### Batch A — core types + context switch

1. **`include/cpu.h`** — `NR_CPUS` (2, matching `-smp 2`),
   `struct cpu_context` (x19..x28/fp/lr/sp, offsets mirrored in
   `switch.S`), `struct per_cpu {current, sched_ctx, online,
   in_irq, need_resched, switches}`, `cpu_id()` via MPIDR Aff0.
2. **`include/task.h`** — task states (`UNUSED/READY/RUNNING/
   SLEEPING/BLOCKED/DEAD`), `struct task` (ctx at offset 0, prio,
   FIFO `rq_key`, quantum, sleep deadline, wait-queue link),
   lifecycle + scheduling + blocking API declarations.
3. **`arch/aarch64/switch.S`** — `cpu_switch_to(from, to)`: saves
   x19..x28/fp/lr/sp into `from->ctx`, loads them from `to->ctx`,
   `ret`s into the saved lr. Caller-saved regs need nothing (C call
   boundary); PSTATE belongs to whatever context resumes.
4. **`kernel/task.c`** — static pool (`MAX_TASKS`, 16 KiB stacks),
   `task_create()` primes a context by hand (sp = stack top, lr =
   trampoline), `task_first_entry()` calls `fn(arg)` then exits;
   `task_exit/yield`; blocking primitives.

> Commit: `Add task structs, kernel stacks, spinlocks and AArch64 context switch`

### Batch B — scheduler core + SMP plumbing

5. **`include/spinlock.h`** — LDAXR/STXR test-and-set with
   acquire/release; irqsave variants (all scheduler paths use them).
6. **`kernel/sched.c`** — shared-run-queue pick (lowest prio number,
   ties by arrival ticket => round-robin among equals; one global
   queue doubles as basic load balancing), `sched_tick()` (wakes
   expired sleepers, quantum expiry sets `need_resched`),
   `sched_post_irq()` preemption point AFTER all EOIs are done so no
   interrupt is ever active across a switch.
7. **GIC/time splits** — `gic_cpu_init()` (banked CPU interface)
   separated from one-shot distributor init; `time_cpu_init()`
   arms each cpu's virtual timer + enables its banked PPI27;
   `jiffies_inc()` hand-rolled LL/SC (freestanding builds get no
   `__atomic` helpers).
8. **`kernel/smp.c` + `arch/aarch64/sec_entry.S` + `start.S`**
   rewrite — see debugging saga #1: release path ended up being
   **PSCI_CPU_ON** through the FDT-probed hvc conduit, not a parking
   pen. `vmm_cpu_activate()` re-applies the boot cpu's MAIR/TCR/
   TTBRs on a secondary; `vmm_sync_kernel_to_ram()` hands dirty
   tables to cache-cold cores (see saga #2). Platform probe gained
   `/psci` parsing (`method`, `cpu_on` fn id).
9. **Exception hook** — IRQ/FIQ windows set/clear `in_irq` around
   dispatch and run the preemption point afterwards.

> Commits: `Add priority round-robin scheduler core and QEMU-virt SMP bring-up`
> (+ your `Checkpoint phase 4 work-in-progress`)

### Batch C — blocking primitives + milestone demo

10. **`wait_sleep_when(cond, ctx, wq)`** — predicate evaluated under
    the scheduler lock and enqueued atomically, so a wake between
    "check" and "sleep" is impossible; `msleep()` (deadline walk in
    the tick), `task_yield`, `wait_wake_all` (with preempt hint).
11. **`kernel/selftest_sched.c`** — 1000-round strict-turn ping-pong
    between two helpers (sequence arithmetic catches lost wakeups,
    double-runs, out-of-order switches), deadline derived from the
    raw counter so it fires even if ticks die; plus the persistent
    demo pair printing `ping:/pong:` rounds tagged with their cpu.
12. **`kernel/main.c`** final flow: ... phase-3 selftests ->
    `sched_init` -> `smp_init` -> `sched_selftest` -> banner
    `[OK] mobile_phone_os phase 4` -> demo threads -> a low-priority
    "housekeep" task (tasklet drain + once-per-second uptime line +
    `msleep(2)`) -> boot context retires into cpu0's scheduler loop.

> Commit: `Complete phase 4: ...` (this batch)

## Debugging saga (real bugs found, in discovery order)

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | secondary never reaches its park loop ("P" probe never prints) | QEMU virt does NOT run secondaries through `_start`: they sit in the firmware **PSCI parking pen**; the DTB `/psci` node says `method="hvc"`, `cpu_on=0xc4000003` | dropped spin-table assumptions entirely; `psci_call()` issues HVC CPU_ON from EL1 (QEMU intercepts it), entry via `sec_entry.S` |
| 2 | random single-threaded stalls inside the image-wide `dc cvac` sweep (and elsewhere) under `-smp 2` | guest cache-maintenance instructions are flaky under QEMU's MTTCG | removed `dc cvac` entirely: push just the dirty pages (page tables + stack mailboxes) to DRAM through the **uncached device window** instead |
| 3 | data abort, DFSC=level-1 translation fault at `devmap(lower_l0)` | the device window only mapped PA `[0,1GB)` — but RAM begins *at* 1 GB, and phase-2 never exercised devmap | window extended over RAM (with a loud coherence warning in the comment) |
| 4 | sync-exception livelock at `v_sync` with SP=`0xffffffffc9e84310` (caught live via monitor) | classic switch race: schedule() marked the outgoing task READY and released the lock *before* `cpu_switch_to` finished saving it — another cpu picked up a half-saved context | first attempt (lock held across the switch) traded it for bug #5 |
| 5 | helpers never ran; cpu1 spun forever on the state lock inside `wait_sleep_when` | lock-spanning-switch deadlock: the lock belonged to a *frozen frame* of the outgoing task; the incoming task's next blocking call locked against it | **redesign**: xv6-style per-CPU scheduler contexts — tasks only ever park into their own cpu's scheduler (`sched_park`), which alone picks and loads the next task; a context is written only while its owner executes |
| 6 | crashes reported as unrelated silent hangs at random later points | reporter blindness: a fault while any dead context held the UART tx-lock made the fault handler itself block forever | `uart_panic_mode()` raw path: exception dumps and panics print without locking |
| 7 | spurious "blocking in irq" panic from demo threads | per-cpu `in_irq` lies once a task switched in during an irq window migrates | blocking precondition reduced to the accurate invariant: a current task exists |

Investigation techniques that paid off: raw-UART probe bytes from
`start.S` (proved cpu1 never ran our entry), FDT property dumps for
`/psci`, QEMU monitor `cpu 1` + `info registers` symbolized against
`nm` (turned "mystery hangs" into named code paths: the spinlock loop,
the vector prologue with a garbage SP).

Design notes worth keeping:

- Preemption point lives *after* `irq_dispatch()` returns (everything
  EOIed), so parking an interrupted task can never strand controller
  state; the trap frame sits above the saved sp on the same stack and
  erets correctly whenever that task resumes.
- Wakeup latency for a sleeping cpu is bounded by the timer tick
  (<= 10 ms): there are no IPI rescheds yet; SGI infrastructure from
  phase 3 makes adding them mechanical.
- The per-cpu scheduler loop IS the WFI idle (item 23): with an empty
  ready set it drops to WFI until the next tick/device interrupt.

## Verification status

Per coordination decision (phase 5 developed in parallel in this
tree by a second stream), final integration builds and the smoke run
are deferred until the project-complete point. The last verified-good
behavior before the freeze: full boot through both phase-3 selftest
groups, `smp: cpu1 online` via PSCI, `selftest: sched (2000 cross-cpu
handoffs) ok`, phase-4 banner, alternating `ping:/pong:` rounds, UART
echo, and per-second uptime lines — reproduced across repeated runs
once saga items #2/#3/#5 were fixed. `make test` greps banner +
uptime + echoed `phase4rx` + both thread prefixes (harness takes the
expected phase number as argv).

Notes carried forward:

- GICv3 backend still deferred (loud refusal on detection).
- Per-cpu areas are a plain indexed struct; no fancy `.percpu`
  sections yet.
- No priority inheritance or lock-debug tooling on `task_state_lock`.
- W^X enforcement still open (phase-2 note); `vmm_unmap` still leaks
  empty intermediate tables.
- Device window now aliases RAM uncached: safe for the SMP push hack
  and early MMIO, dangerous for live cached data — the comment in
  `build_upper_windows` says so in capitals.

> Commit: `Add phase 4 implementation log`
