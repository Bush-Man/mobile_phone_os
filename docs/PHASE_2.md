# Phase 2 — Memory Management (Implementation Log)

Milestone reached: physical frame allocator, 4-level page-table API with
TTBR1 higher-half windows, and a kernel heap with debug poisoning all
pass their boot self-tests; `make test` prints
`[OK] mobile_phone_os phase 2`.

## Order of files written

### Batch A — core types + physical allocator

1. **`include/mm/types.h`** — `paddr_t`/`vaddr_t`, `PAGE_SIZE/PAGE_SHIFT`,
   `KiB/MiB/GiB`, `ALIGN_UP/ALIGN_DOWN/IS_ALIGNED`, `ARRAY_SIZE`.
2. **`include/mm/pmm.h`** — `struct pmm_stats {total,reserved,free}`;
   API: `pmm_init`, `pmm_alloc`, `pmm_free`, `pmm_stats_get`.
3. **`mm/pmm.c`** — LIFO free list threaded through the free frames
   themselves (first word of each free frame = next free frame, zero
   metadata). `pmm_init(ram_base, ram_size)` skips the kernel image
   `[ALIGN_DOWN(_start), ALIGN_UP(_end))`; alloc pops the head; free
   pushes back.

> Commit: `Add core memory types and physical frame allocator`

### Batch B — page tables + higher-half windows

4. **`include/mm/vmm.h`** — VM flags (`VM_READ/WRITE/EXEC/USER/DEVICE`);
   window bases (final values):
   - `KERN_DMAP_BASE    = UBASE + 0x000000000000` (direct map of RAM)
   - `KERN_TEST_BASE    = UBASE + 0x008000000000`
   - `KERN_HEAP_BASE    = UBASE + 0x010000000000`
   - `KERN_DEVICE_BASE  = UBASE + 0x800000000000`
   where `UBASE = 0xFFFF000000000000` (TTBR1 upper half);
   helpers `vmm_dmap()/vmm_devmap()`; API `vmm_init/vmm_map/
   vmm_unmap/vmm_translate`.
5. **`mm/vmm.c`** — descriptor bits (`PTE_VALID`, table = `0b11`,
   block/page = `0b01` — see debugging note #3!); AttrIdx 0=Normal WB,
   1=Device-nGnRE via MAIR idx; AP encoding (kernel RW=`00`, user RW=`01`,
   kernel RO=`10`, user RO=`11`); SH inner; AF; nG for user pages;
   PXN/UXN per exec+privilege. Statics: `lower_l0/lower_l1/upper_l0`
   (all 4 KiB aligned). Functions:
   - `table_ptr(desc)` — extracts table PA via `PT_ADDR_MASK`
     (**must mask attribute bits 0..11 too** — bug #3 below)
   - `leaf_desc/block_desc/table_desc` encoders
   - `tlb_flush_all()` (`tlbi vmalle1is`), `tlb_flush_va()`
   - `vmm_map(va, pa, flags)` — walks L0→L2, allocating zeroed tables
     from PMM on demand, writes L3 leaf, per-map flush
   - `vmm_unmap`, `vmm_translate` — mirrored walkers
   - `build_lower_identity()` — devices below RAM as GB blocks + RAM as
     GB blocks, hung off lower_l0[0]→lower_l1
   - `build_upper_windows()` — dmap + device window GB blocks
   - `vmm_init(plat)` — pmm_init → build both roots → MAIR →
     TTBR0/TTBR1 → TCR (T0SZ=T1SZ=16) → dsb/isb → SCTLR |= M|C|I →
     `tlbi vmalle1is`
6. **Deleted**: `arch/aarch64/mmu.c`, `include/mmu.h` (superseded).
7. **`Makefile`** — `SRCS_C` extended with `mm/*.c`.
8. **`kernel/main.c`** — calls `vmm_init(&plat)` instead of mmu_enable.

> Commit: `Add 4-level page table API with kernel higher-half direct map and device windows`

### Batch C — kernel heap

9. **`include/mm/kheap.h`** — `struct kheap_stats`; `kmalloc/kzalloc/
   kfree/kheap_stats_get`.
10. **`mm/kheap.c`** — 8 power-of-two size classes (16..2048) with
    per-class LIFO freelists threaded through chunk bodies; large
    allocations (>2048) as dedicated multi-page spans; 16-byte chunk
    header `{magic, req, cls, npages}`; end canary `0xCAFEF00D` after
    the requested span, verified on free; `KHEAP_DEBUG` fill patterns
    (alloc=0xAA, freed poison=0x5A); double-free detection via magic.
11. **`lib/string.c`** + prototypes in `include/lib.h` — freestanding
    `memset/memcpy/memmove` (GCC lowers `__builtin_memset` to a real
    call for non-constant sizes).

> Commit: `Add kernel heap with slab size classes, canaries and debug poisoning`

### Batch D — self-tests + wiring

12. **`mm/selftest.c`** — `mem_selftest()`:
    - `test_pmm`: alloc/free balance restores stats
    - `test_vmm`: map 4 pages in two distant TTBR1 windows, pattern
      write/readback through the mappings, software-walk agreement,
      cross-window aliasing of one frame, unmap + translate-miss
    - `test_kheap`: 400 mixed slab/large allocations with byte-pattern
      fills, scrambled frees, survivor verification, stats balance
13. **`kernel/main.c`** final sequence: uart → el → vectors → platform
    → relocation sanity → vmm_init → mem_selftest → stats print →
    banner → wfi. Smoke-test grep updated to phase 2.

> Commits: `Complete phase 2...`, plus intermediate fixes pushed along the way.

## Debugging saga (real bugs found, in discovery order)

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | link error: undefined `memset` | GCC lowers `__builtin_memset` to a libc call for non-constant sizes | added `lib/string.c` |
| 2 | `el.o` clobbered by `el.S` (phase-1 leftover) | two sources producing the same object name | renamed asm file to `el_enter.S` |
| 3 | data abort writing a freshly mapped test window | `table_ptr()` masked with the COMPLEMENT of the 48-bit VA mask — kept garbage high bits, dropped the physical address; every chained walk followed bogus pointers | `desc & PT_ADDR_MASK` |
| 4 | same symptom persisted | earlier PROP-era confusion; actually two separate bugs stacked (see #5) | fixed `fdt.c` PROP stride from phase-1 debugging |
| 5 | FDT probe returned null props | node offsets are blob-relative; `fdt_getprop` treated them struct-block-relative, double-counting `off_dt_struct` | cursor computed from `hdr + node_off` |
| 6 | UART probe missed the console node | this QEMU version places `pl011@9000000` directly under root (no `/soc`) | candidate-path list in `probe_serial` |
| 7 | UART base printed as 0x0 | root uses `#address-cells=<2>`; reading one u32 grabbed the zero high word | decode via `cells_u64(reg, #address-cells)` |

## Debugging saga part 2 — the "impossible" walk faults

After the above fixes the smoke test still died with a level-3
translation fault on the first selftest store. Isolation steps that
finally cracked it:

1. **Host-side harness** (`tests/fdt_host_test.c` + throwaway dumpers)
   validated the FDT parser against the real blob without flashing.
2. **Temporary serial probes inside `mmu_enable`/`vmm_map`** narrowed
   each failure to an exact instruction.
3. **QEMU monitor (`xp`)** read raw physical memory at the walker's
   exact fetch addresses and matched our dumps — bytes were right.
4. **`AT S1E1R` + PAR_EL1** from the fault handler confirmed genuine
   level-N translation faults.
5. Root causes, once visible:
   - **T0SZ/T1SZ mismatch**: TnSZ=16 forces level-0 start; our first
     attempt used a bare L1-root table (valid only for TnSZ=25) →
     instant fault on enable. Final design keeps TnSZ=16 with true
     4-level roots.
   - **Block-vs-table bit confusion**: `PTE_BLOCK (1<<1)` produced
     `0b11` = table descriptors, so walks descended into garbage.
     Blocks are `VALID` alone (`0b01`).
   - **Misaligned table pointers**: `table_ptr` kept attribute bits,
     yielding byte-shifted table addresses.
   - **RWX LOAD segment warning**: fixed with PHDRS text/data split.

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
bringup: memory management up (TTBR0/TTBR1 + caches on)
selftest: pmm .............. ok
selftest: vmm .............. ok
selftest: kheap (400 allocs) . ok
mm: 30786/31967 frames free (1156 KiB reserved)
mm: 30786/31967 frames free, heap 400/400 allocs
[OK] mobile_phone_os phase 2
```

Notes carried forward:

- Heap arena currently uses identity-mapped frames directly (PA==VA);
  moving it into the higher-half window requires post-enable mapping
  reliability, deferred to the phase-3 paging rewrite.
- `vmm_unmap` does not yet reclaim empty intermediate tables.
- W^X is not enforced yet (kernel text/data share one Normal region);
  planned for phase 3 alongside per-page permissions.

> Commit: `Add phase 2 implementation log`
