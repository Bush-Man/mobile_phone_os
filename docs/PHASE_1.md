# Phase 1 — CPU Bring-Up & Platform Discovery (Implementation Log)

Milestone reached: kernel reports its exception level, installs an EL1
vector table, parses the device tree (model / RAM size / UART base /
bootargs), passes a relocation sanity check, enables the MMU with an
identity map plus caches, and prints `[OK] mobile_phone_os phase 1`.
Smoke test command: `make test`.

## Order of files written

### Batch B — exception levels, vectors, panic

1. **`include/el.h`**
   - `static inline uint64_t el_current()` — reads `CurrentEL`, shifts right 2
   - prototypes: `el_drop_to_el1`, `el_enter_el1_from_el2`,
     `el_enter_el1_from_el3`
2. **`arch/aarch64/el_enter.S`** (first drafted as `el.S`; renamed because
   `el.c`/`el.S` both produced `el.o` and clobbered each other)
   - `el_enter_el1_from_el2(target_pc)` — `SPSR_EL2 = 0x3C5`
     (EL1h, DAIF masked), `ELR_EL2 = x0`, `eret`
   - `el_enter_el1_from_el3(target_pc)` — same via `SPSR_EL3`/`ELR_EL3`
3. **`arch/aarch64/el.c`** — constants `SCR_NS(bit0)|SCR_RW(bit10)`,
   `HCR_RW(bit31)`; single function:
   - `el_drop_to_el1()` — EL1: no-op return; EL2: `CNTVOFF_EL2=0`,
     `CNTHCTL_EL2=3<<10`, `HCR_EL2.RW=1`, then trampoline; EL3:
     `SCR_EL3 = NS|RW`, trampoline; continues after the call via a
     computed-goto continuation label + `isb`
4. **`include/panic.h`** — `panic(const char*)` noreturn,
   `__stack_chk_fail()` noreturn
5. **`kernel/panic.c`** — `__stack_chk_guard` (later given a non-zero
   `.data` initializer so protected functions are safe before any init),
   `panic()` (`daifset 0xf` → print → `wfe` loop),
   `__stack_chk_fail()`
6. **`include/exceptions.h`**
   - `struct trap_frame { uint64_t regs[31]; esr; elr; spsr; sp; }`
     (280 bytes; offsets mirrored exactly by the asm stubs)
   - `enum exc_kind { EXC_SYNC, EXC_IRQ, EXC_FIQ, EXC_SERROR }`
   - prototypes `vectors_init`, `exceptions_handler`
7. **`arch/aarch64/vectors.S`** — section `.vectors,"ax"`, `.align 11`
   (VBAR needs 2 KiB alignment); `STUB kind` macro:
   allocate 280 bytes → stp all GPRs → read ESR/ELR/SPSR into scratch →
   store them + interrupted SP (computed as `sp+280`) →
   `bl exceptions_handler(sp, kind)` → restore sysregs first, then GPRs,
   LR last → `add sp,#280` → `eret`. Sixteen entries (SP0/SPx/A64/A32 ×
   sync/IRQ/FIQ/SError); globals `vectors_begin`, `vectors_end`.
   *First draft had a broken restore path (orig-SP capture raced its own
   save slot) and was fully rewritten before ever being built.*
8. **`kernel/exceptions.c`**
   - `vectors_init()` — writes VBAR_EL1 + `isb`
   - `ec_name(uint64_t)` — small EC classifier (unknown/svc/instr abort/
     data abort/alignment…)
   - `dump_regs()` — x0..x30 grid
   - `exceptions_handler(tf, kind)` — reads FAR_EL1, prints kind, ESR/EC,
     ELR/FAR/SPSR/SP, register dump, then `panic()`
9. **`linker.ld` edit** — added `PHDRS` (text R+X FLAGS(5), data R+W
   FLAGS(6)) and a `.vectors :text` section right after `.text.boot`;
   this also eliminated binutils' earlier `RWX LOAD segment` warning
10. **`start.S edit`** — `msr daifset, #0xf` as the very first instruction
    (deterministic masking until Phase 3 owns interrupts)

> Commit: `Add exception vector table, trap frame handling, EL drop path and panic support`

### Batch C — FDT parser and platform probe

11. **`include/fdt.h`** — `FDT_MAGIC 0xd00dfeed`; `enum fdt_token`;
    `struct fdt_header` (all big-endian fields);
    `struct fdt {hdr, strings, st}`; API `fdt_init`, `fdt_u32`,
    `fdt_find_node`, `fdt_getprop`. Node paths accept per-segment
    trailing `*` wildcards (`"/memory*"`, `"/soc/pl011*"`).
12. **`lib/fdt.c`** — local string helpers (`s_len/s_cmp/s_ncmp/s_chr`);
    byte-wise big-endian `fdt_u32`; `fdt_init` (magic check, derive
    strings/struct pointers); `seg_match`; `after_name` (name padding to
    u32 boundary); `skip_subtree` (depth-counting); `first_seg_len`;
    recursive `scan_contents` (props skipped, children matched or
    subtree-skipped); `fdt_find_node`; `fdt_getprop`.
13. **`include/platform.h`** — `PLATFORM_MODEL_MAX/BOOTARGS_MAX`;
    `struct platform_info {model, ram_base, ram_size, uart_base,
    boot_args, has_uart, has_boot_args}`; `platform_probe`,
    `platform_self`.
14. **`kernel/platform.c`** — extern `_binary_platform_qemu_virt_dtb_*`;
    `copy_str`, `cells_u64` (u64 from N BE cells), `root_cells`
    (#address-cells/#size-cells with defaults 2/2), `probe_memory`
    (reg decoded per parent cell counts), `probe_serial` (multi-path
    candidates), `probe_chosen` (bootargs), `platform_probe`,
    `platform_self` (embedded blob).
15. **`Makefile` edits** — `$(BUILD)/fdt_blob.o` rule:
    `objcopy -I binary -O elf64-littleaarch64 --rename-section
    .data=.rodata.fdt,alloc,load,readonly,data,contents`; `make dtb`
    regenerates the blob via `-machine dumpdtb=`; `OBJS += fdt_blob.o`.

Build issue: freestanding build lacks `NULL` transitively → added
`<stddef.h>` to `fdt.c`.

> Commit: `Add FDT parser and platform info probe with embedded QEMU virt DTB`

### Batch D — MMU, caches, stack protector, bring-up sequence

16. **`include/mmu.h`** — `mmu_enable(ram_base, ram_size)`,
    `mmu_active()`.
17. **`arch/aarch64/mmu.c`** — attribute indexes (NORMAL=0, DEVICE=1);
    descriptor bits (`PTE_VALID`, ATTRIDX<<2, SH_INNER, AF, PXN, UXN);
    `static uint64_t l1_table[512] __attribute__((aligned(4096)))`;
    `map_block()` (1 GiB block at L1 index); `mmu_enable()`:
      - everything below `ram_base` (< 4 GiB cap) → Device-nGnRE, PXN
      - RAM rounded up to GiB boundary → Normal WB, executable
      - `MAIR_EL1 = (0xFF<<0)|(0x04<<8)`; `TCR_EL1` with **T0SZ/T1SZ=25**
        (39-bit VA ⇒ walk starts at level 1, matching the L1-block-only
        table), TG0/TG1=4 KiB, WB walk, inner-shareable;
      - `TTBR0_EL1 = l1_table`, `dsb sy`, `ic iallu`, then
        `SCTLR |= M|C|I`, `isb`.
    `mmu_active()` re-reads SCTLR.M.
18. **`kernel/main.c` rewrite** — bring-up order:
    uart → `el_drop_to_el1()` + report → `vectors_init()` +
    report → `platform_self()` + model/RAM/UART/bootargs report →
    relocation sanity (`_start == 0x40000000` else `panic`) →
    `mmu_enable()` + SCTLR.M verification → phase banner → `wfi`.
19. **`kernel/panic.c` edit** — guard initialised to a non-zero constant
    (stack protector is active from the very first protected function).
20. **`lib/printf.c` edit** — `%s` guards NULL → `(null)`.
21. **`Makefile` edits** — CFLAGS swap `-fno-stack-protector` →
    `-fstack-protector-strong -mstack-protector-guard=global`;
    smoke-test grep now expects the phase 1 banner;
    `SRCS_C` extended with `arch/aarch64/*.c` (was silently missing!).

> Commit: `Enable MMU with identity mapping and caches; full phase 1 bring-up sequence passes`

## Debug iterations worth remembering

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | link: undefined `el_drop_to_el1`, `mmu_enable` | `SRCS_C` wildcard never included `arch/aarch64/*.c`; `el.c`+`el.S` also collided on `el.o` | extend wildcard; rename asm file to `el_enter.S` |
| 2 | FDT: every node lookup returned garbage tokens | PROP record misread as `[len][nameoff][value]` when the format is `[token][len][nameoff][value]` — my code took `len` from the token word itself | fixed stride math in `skip_subtree`, `scan_contents`, `fdt_getprop` |
| 3 | props still null after fix 2 | `node_off` is blob-relative (includes `off_dt_struct`) but `fdt_getprop` indexed from the struct block, double-counting the offset | compute cursor as `hdr + node_off` |
| 4 | UART probe missed the console | this QEMU version places `pl011@9000000` directly under root — there is **no `/soc`** | candidate-path list: `/pl011*`, `/soc/pl011*`, `/serial*`, `/soc/serial*` |
| 5 | UART base printed as `0x0` | root uses `#address-cells=<2>`; reading one u32 grabbed the zero high word | decode base with `cells_u64(reg, #address-cells)` |
| 6 | silent lockup at MMU enable | `T0SZ=16` forces a level-0 walk; L0 entries may only be tables, ours were "blocks" → instant translation fault, and vector fetch faulted too (same broken mapping) hence zero output | `T0SZ/T1SZ = 25` (39-bit VA, start at level 1) |
| 7 | silent lockup persisted | `PTE_BLOCK (1<<1)` turned entries into `0b11` = **table descriptors**, so walks descended into garbage memory | block descriptors at L1/L2 are just `VALID` (`0b01`); removed the bit |

Debug technique used for 6/7: temporary `uart_dbg` probes inside
`mmu_enable` narrowed the hang to the `SCTLR.M` write moment; removed
after the fix. Host-side harness `tests/fdt_host_test.c` (plus throwaway
dumpers in `/tmp/opencode/`) validated `lib/fdt.c` against
`platform/qemu-virt.dtb` without flashing the kernel each time.

## Verification result

```
$ make test
SMOKE TEST: PASS

mobile_phone_os kernel
bringup: running at EL1 (boot EL1)
bringup: vectors installed at 0x40000800
platform: model "linux,dummy-virt"
platform: RAM 128 MiB @ 0x40000000
platform: console UART @ 0x9000000
image: linked at 0x40000000, ends at 0x4014xxxx (~1290 KiB)
bringup: MMU enabled, caches on (SCTLR.M=1)
[OK] mobile_phone_os phase 1
```

Notes carried forward:
- `boot EL1` on QEMU; the EL3/EL2 drop paths compile but stay untested
  until real firmware chains run (RPi4/PinePhone bring-up).
- The embedded-DTB trick exists because QEMU zeroes registers on direct
  ELF boot; real boards hand us the pointer in `x1` and `kmain` already
  accepts it. Regenerate the blob with `make dtb` whenever `-cpu`/`-m`
  change (the memory node must match actual RAM).
- Phase 2 replaces the single L1 block table with a real page allocator
  and 4-level mappings; W^X splitting arrives with it.

> Commit: `Add phase 1 implementation log`
