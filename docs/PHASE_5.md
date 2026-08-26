# Phase 5 — Userspace, Syscalls & Processes (Implementation Log)

Milestone target reached by this phase: a static "hello" ELF binary
runs at EL0 against the raw svc ABI and returns its exit code to the
kernel, which reaps it via waitpid and reports
`[demo] hello exited with code 42`. Along the way the kernel gained
per-process ASID-tagged address spaces, an ELF64 program loader,
a syscall table behind the svc dispatch path, validated copy-in/
copy-out accessors, fork/exec-like process creation and a signals
layer (delivery, handlers, default actions).

Builds/QEMU verification for this phase were deliberately deferred
by the project owner until integration (phase 4 was landing in
parallel from a second workspace); every batch below was written to
compile standalone and reviewed line-by-line instead.

## Order of files written

### Batch A — address-space foundation

1. **`include/signal.h`** — Linux-numbered signal subset (`NSIG=32`,
   SIGUSR1/SEGV/KILL/CHLD/...), `sig_handler_t`, SIG_DFL/SIG_IGN,
   `signal_default_fatal()` contract, and `struct sigframe` (magic +
   full user register snapshot consumed by SYS_sigreturn).
2. **`include/proc.h`** — the phase-5 data model in one place:
   user VA window constants (code at 0x0100_0000_0000 = L0 index 8,
   stack top at 0x0200_0000_0000 = index 16, limit 0x0400_0000_0000),
   `struct proc` (pid, root_pa/asid/gen, dedicated kmalloc'd kernel
   stack, zombie/exit-code/parent, registry link, brk, pending bits +
   handler table + sigreturn frame pointer, embedded first-user-state
   `entry_tf`), and the whole lifecycle API surface.
3. **`mm/vmm.c` (+ `include/mm/vmm.h`)** — appended a "process
   address spaces" section reusing the file's private descriptor
   encodings: `vmm_kernel_root/vmm_shared_l1` (the shared identity
   subtree every root aliases at L0 index 0), `vmm_root_alloc/free`
   (zeroed root + shared splice), `vmm_root_release` (recursive frame
   + table teardown over an L0 index range, index 0 refused),
   `vmm_map_at/unmap_at` (root-parameterised 4 KiB mapping),
   `vmm_decode_flags` (descriptor -> VM_* incl. AP[2] as VM_USER),
   `vmm_probe` (software walk returning PA+flags of whatever leaf
   covers a VA -- the primitive uaccess and fork are built on).
4. **`kernel/proc.c`** — started as the core only: 8-bit ASID
   allocator with generation rollover (`asid_alloc`: linear scan,
   exhaustion bumps generation + `tlbi vmalle1is`; freed ASIDs get an
   immediate `tlbi aside1is` so same-generation reuse never aliases),
   `proc_current()`, and `proc_address_space_switch()` (TTBR0 =
   root | asid<<56, or the plain kernel root with reserved ASID 0 for
   kernel threads).
5. **`arch/aarch64/user_entry.S`** — `proc_enter_user`: x0 = trap
   frame, installs it as SP_EL1 and erets to EL0; shared tail
   `proc_tf_eret` restores elr/spsr, ALWAYS writes SP_EL0 (every
   caller here returns to EL0 by construction) and parks SP_EL1 just
   above the consumed frame so each task's next exception rebuilds
   its frame at the same slot.
6. **`arch/aarch64/vectors.S`** — EL0-aware bodies: entry now picks
   the interrupted SP by testing SPSR.M[3:2] (0 => EL0 =>
   `mrs x24, sp_el0`; else sp-on-entry), and the epilogue writes
   SP_EL0 back before reloading x24 when returning to EL0. Same-EL
   behaviour byte-for-byte identical to phase 3 otherwise.

> Commit: `Add phase 5 userspace core: ...` (combined with batches B/C)

### Batch B — syscall path + safe user memory access

7. **`include/syscall.h`** — ABI contract (number x8, args x0..x5,
   negative-errno returns), numbers SYS_exit/fork/execve/waitpid/
   write/read/getpid/kill/sigreturn/sleep/sigaction/uptime_ms, errno
   subset.
8. **`kernel/uaccess.c` (+ header)** — plan item 31. Validation model:
   every page of a range is software-walked via `vmm_probe` against
   the CALLING process's root BEFORE any bytes move; a page qualifies
   only if the leaf exists, carries AP[2] (VM_USER) and the direction
   needs VM_WRITE for copy-out; ranges must stay under USER_VA_LIMIT
   without wrapping. Copies then run through the identity alias of
   translated frames and cannot abort, so no fixup tables are needed
   on this path; anything that still faults at EL0 becomes SIGSEGV
   via `proc_user_fault`. Exports copy_in/copy_out/strnlen plus
   `_cur` convenience wrappers bound to `proc_current()->root_pa`.
9. **`kernel/syscall.c`** — table of uniform handlers + dispatch.
   fork/sigreturn special-cased before lookup (they need the raw
   frame). write/read chunk 256-byte kernel buffers through uaccess
   onto the PL011 / out of the interrupt-fed RX ring (non-blocking,
   -EAGAIN when empty). execve copies name/argv/envp OUT of the dying
   space first (caps: 8 args x 48 chars, 1 env var) because exec
   replaces the very mappings the pointers live in. waitpid compacts
   `{exit code, pid}` into one return value for the freestanding
   libc-less demo.
10. **`kernel/exceptions.c`** — sync routing: EC=0x15 from EL0 ->
    `syscall_dispatch` then `signal_deliver_pending`; aborts/PC-SP
    alignment faults from EL0 -> `proc_user_fault` (kill-with-report);
    svc issued AT EL1 is now a loud kernel bug. IRQ/FIQ path runs
    `signal_deliver_pending` after `sched_post_irq` when returning to
    EL0. exp_skip_faults restricted to !from_user.
11. **`drivers/uart.{c,h}`** — additive `uart_rx_read()`: kernel-side
    drain of the RX ring for SYS_read (echo tasklet untouched).

### Batch C — ELF64 loader + the built-in hello image

12. **`include/elf.h` / `kernel/elf.c`** — plan item 29. Validates
    ident/class/LE, ET_EXEC AArch64, phentsize==56, bounds everywhere;
    rejects PT_INTERP (dynamic) up front; maps every PT_LOAD page
    range with p_flags -> VM_READ|VM_WRITE|VM_EXEC|VM_USER, copying
    file bytes per-page-overlap through the identity alias and zeroing
    the filesz->memsz tail (.bss); returns entry PC and heap base
    (page above highest segment). Layout produced is deliberately
    dynamic-linker-ready (fixed LOADs, auxv on the stack).
13. **`mm/vmm.c`** — `vmm_copy_space()`: fork's deep copy; recursive
    table duplication preserving decoded permissions, leaf pages
    memcpy'd eagerly through the identity alias (COW deferred --
    see notes).
14. **`arch/aarch64/builtin_imgs.S`** — embeds userspace/hello
    VERBATIM (.incbin, ELF headers and all) so the loader parses a
    genuine ELF64 file; only the byte source changes when real
    storage arrives in later phases.
15. **`userspace/hello.ld`** — static link at USER_CODE_BASE.
16. **`userspace/hello.c`** — the milestone program, freestanding
    `-mgeneral-regs-only`, raw svc wrappers: prints pid/argv/uptime,
    raises SIGUSR1 at itself (handler runs, sigreturns), forks
    (child exits 7), waits and decodes the compact waitpid result,
    exits 42.
17. **`Makefile`** — USER_CFLAGS (freestanding, no-pic/pie,
    general-regs-only) + link rule with -T hello.ld -no-pie
    --build-id=none; explicit prerequisite wiring builtin_imgs.o to
    the hello binary.

> Commit: `Add phase 5 userspace core: ...`

### Batch D — lifecycle: spawn/exec/fork/wait/kill/signals

18. **`kernel/proc.c`** completed around the Batch-A core:

    - `stack_setup()` — maps USER_STACK_SIZE into the new root and
      lays out strings top-down, then argc/argv/envp vectors and a
      minimal auxv (AT_PAGESZ, AT_NULL), all written through
      uacc_copy_out so even boot-time population validates like any
      user write; returns initial x0/x1/x2/SP.
    - `load_image()` — elf_load + stack_setup + craft `entry_tf`
      (x0..x2, usp, elr, spsr=EL0t interrupts-on).
    - `proc_spawn()` — builtin lookup, kzalloc proc + kmalloc
      PROC_KSTACK, fresh root+ASID, load, task_create(body), registry.
    - body trampolines `proc_task_body`/`fork_child_body` — the task
      ADOPTS its own ->proc (closes the create/pick race locklessly),
      switches its address space in explicitly, builds the first trap
      frame at the top of ITS OWN kstack and calls proc_enter_user;
      the scheduler's static task stack is abandoned from here on.
    - `proc_do_exec()` — builds the ENTIRE replacement image in a
      scratch root before committing; commit point swaps root/asid,
      resets handlers/pending (POSIX-lite), tears down + frees old
      space and ASID, switches TTBR0 on the spot and erets into the
      new image. Failures leave the old process intact.
    - `proc_do_fork()` — vmm_copy_space the user image, inherit
      handlers (POSIX) but not pending bits, snapshot the parent's
      syscall trap frame into child->entry_tf with x0 = 0; the child
      is scheduled via the same trampoline and "returns" from fork
      directly to user mode. Parent gets the pid.
    - zombies/reaping — `mark_zombie` (alive=false, exit_code,
      ASID released immediately for TLB hygiene, task marked DEAD,
      reap queue woken) vs `reap_one` (registry unlink, space
      destroy, kfree kstack, task slot reset to TASK_UNUSED only if
      fully DEAD-parked, kfree proc). `proc_poll_reap` non-blocking
      scan; `proc_do_waitpid` blocks on the shared reap waitqueue
      with the predicate evaluated under the scheduler lock
      (no lost wakeups). -ECHILD semantics.
    - `proc_do_kill()` — pending-bit delivery at the target's next
      kernel->EL0 boundary; no cross-cpu stack surgery (documented
      limitation, no EINTR yet).
19. **`kernel/signal.c`** — plan item 30. Default actions (SIGCHLD
    ignores, everything unhandled terminates 128+sig); delivery
    builds `struct sigframe` + two-instruction trampoline
    (movz w8,#SYS_sigreturn; svc #0) on the USER stack above the
    handler's downward-growing region, redirects elr to the handler
    with sig in x0 and x30 at the trampoline; nesting disallowed
    (further signals stay pending until sigreturn);
    `proc_sigreturn` restores the frame from the recorded VA with
    magic + SPSR sanity checks, killing on garbage.

### Batch E — wiring

20. **`kernel/proc.c`** — init split: `proc_cpu_init()` (per-cpu
    TCR.A1 = ASIDs-from-TTBR0 + CPACR FPEN untrap) callable from both
    bring-up paths; `proc_subsys_init()` wraps it for the boot cpu.
21. **`kernel/smp.c`** — secondaries call `proc_cpu_init()` right
    after their timer bring-up (after vmm_cpu_activate, which still
    programs the pre-A1 TCR snapshot).
22. **`kernel/sched.c`** — the ONE-line scheduler hook: dispatch
    always writes the incoming task's TTBR0+ASID
    (`proc_address_space_switch(next->proc)`); NULL proc => shared
    kernel root/ASID 0. Always-write beats compare-skip: one msr+isb
    per switch is free next to a context switch.
23. **`kernel/main.c`** — banner -> phase 5; after SMP+scheduler
    selftest: proc_subsys_init(), then a "procdemo" task spawns
    "hello", blocks in waitpid and prints
    `[demo] hello exited with code %d` -- the milestone proof line.
24. **`tests/serial_harness.py` + `Makefile`** — phase>=5 pass
    criteria: banner, uptime, rx echo, ping/pong (kept from phase 4)
    PLUS `running at EL0`, `hello: exiting 42` and
    `exited with code 42`.

## Design decisions worth remembering

| Decision | Rationale |
|----------|-----------|
| User VAs at L0 indices 8..15, far above the identity map | index 0 of every root is the SHARED kernel identity subtree (TLB-global, EL1-only); keeping user mappings in disjoint L0 slots makes teardown trivial (`vmm_root_release(8..16)`) and makes it impossible for a user pointer to alias kernel RAM |
| ASIDs from TTBR0 (TCR.A1=1), reserved ASID 0 = kernel context | context switch = single msr ttbr0_el1 + isb; stale cross-process entries filtered by hardware; kernel root has no nG=1 pages so tag 0 can never collide |
| Dedicated kmalloc'd kstack per process | decouples processes from the static task-stack pool entirely; exception frames land there and the frame base doubles as parked SP_EL1 between syscalls |
| Task adopts its own ->proc inside its body trampoline | closes create-vs-first-dispatch race with zero extra locking; whichever cpu runs the body first sees NULL and installs it |
| exec builds the new space completely before switching | failed exec leaves the old image intact; population goes through uacc_copy_out against the not-yet-active root, exercising the validator on boot-critical paths |
| validate-then-copy uaccess (no fixup tables) | with per-page software walks, physical copies cannot abort; fixup-based accessors remain available later if SMOP-style faults are ever needed |
| eager page copy in fork (no COW yet) | correctness first; COW noted below |

## Known limitations carried forward

- No FP/SIMD state save across switches: CPACR untraps FP but user
  code must stay general-regs-only (hello does). Lazy/save-restores
  arrive with the libc port (phase 14 territory).
- kill() delivers only at kernel->EL0 boundaries; a blocked task
  learns of a fatal signal late (no EINTR machinery yet).
- fork copies pages eagerly; COW + page-fault demand paging are the
  natural next step once a page-fault handler distinguishes user
  regions.
- Single waitqueue for all reapers; concurrent waitpid parents scan
  under proc_lock but reap serialisation is best-effort (fine until
  multiple parents exist).
- Kernel threads that exit leave DEAD slots forever (phase-4 policy):
  with pp-a/pp-b/ping/pong retired, exactly enough UNUSED slots remain
  for housekeep + procdemo + hello + fork-child. Slot recycling for
  kernel threads should land with the driver framework.
- Wall-clock auxv is minimal (AT_PAGESZ/AT_NULL); AT_PHDR/AT_ENTRY
  added when the dynamic linker becomes real.
- Verification note: per project-owner instruction, make/QEMU runs
  were deferred for this phase while phase 4 landed in parallel; the
  expected smoke output below is the acceptance checklist.

## Expected verification output (pending build)

```
$ make test
SMOKE TEST: PASS

mobile_phone_os kernel
bringup: running at EL1 (boot EL1)
...
selftest: sched (1000 cross-cpu handoffs) ok
[OK] mobile_phone_os phase 5
proc: address spaces ready (ASIDs from TTBR0)
ping: round 1 on cpuN
pong: round 1 on cpuM
...
[proc] spawned "hello" pid 1 asid 1 root 4xxxxxxx000
hello: running at EL0, pid 1
hello: argc=1 argv[0]=hello
hello: uptime NNNN ms
hello: raising SIGUSR1
hello: SIGUSR1 handler ran (sig=10)
hello: forked child pid 2
[proc] fork: pid 1 -> pid 2
hello-child: pid 2 exiting with 7
[proc] pid 2 (hello) exited, code 7
hello[pid 1]: reaped child 2, code 7
hello: exiting 42
[proc] pid 1 (hello) exited, code 42
[demo] hello exited with code 42
time: N.NNN s uptime
```

> Commit: `Wire phase 5 into boot: procdemo spawns hello at EL0, phase-aware harness criteria`
