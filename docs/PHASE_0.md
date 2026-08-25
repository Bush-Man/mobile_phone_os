# Phase 0 — Toolchain & Build System (Implementation Log)

Goal reached: cross-compiled AArch64 kernel boots in QEMU `virt` and prints
the `[OK] mobile_phone_os phase 0` banner over the PL011 UART.
Smoke test command: `make test`.

## Environment installed

| Tool | Version | Source |
|------|---------|--------|
| aarch64-linux-gnu-gcc | Debian 15.3.0 | `sudo apt install gcc-aarch64-linux-gnu` |
| binutils (ld, objcopy) | 2.47 | pulled in by the package above |
| gdb-multiarch | 15.x | same apt transaction |
| qemu-system-aarch64 | 11.0.3 | pre-installed |

## Order of files written

### 1. `.gitignore`
Ignores `build/`, `*.o`, `*.elf`, `*.img`, `*.log`.

### 2. `linker.ld`
Written top-to-bottom:
1. `ENTRY(_start)`
2. Load address `. = 0x40000000` (QEMU virt RAM base)
3. Sections in order: `.text.boot` (KEEP, must be first), `.text`, `.rodata`, `.data`
4. Symbols: `__bss_start`, `.bss` (`*(.bss*) *(COMMON)`), `__bss_end` — both ends 16-byte aligned
5. End-of-image symbol `_end`

### 3. `arch/aarch64/start.S` (first code executed)
Symbols and labels in authoring order inside `_start`:
1. `_start` — reads `CurrentEL`, shifts right by 2 → passes EL number in `x0` to C
2. Reads `mpidr_el1`, masks affinity level 0; non-zero cores branch away to park
3. Loads `_stack_top` into `sp` (temporary boot stack)
4. `.Lclear_bss` — stores `xzr` in 8-byte strides from `__bss_start` to `__bss_end`
5. `.Lbss_done` — `bl kmain` with `x0`=EL number, `x1`=DTB pointer from firmware
6. `.Lpark` — `wfe`/`b` loop where parked secondary cores spin forever
7. Section `.bss.stack`: globals `_stack_bottom`, 64 KiB reserved, `_stack_top`

### 4. `include/mmio.h`
Functions (both `static inline`, use acquire/release memory ops):
1. `mmio_read32(uintptr_t addr)` — `ldar`
2. `mmio_write32(uintptr_t addr, uint32_t val)` — `stlr`

> **Commit 1:** `Add phase 0 boot scaffolding: linker script, AArch64 boot stub, MMIO helpers`

### 5. `drivers/uart.h`
Prototypes only: `uart_init`, `uart_putc`, `uart_puts`, `uart_getc`.

### 6. `drivers/uart.c` (PL011 @ `0x09000000`, QEMU virt)
Constants declared first, then functions:
1. Defines: `UART0_BASE`; offsets `UART_DR/_FR/_IBRD/_FBRD/_LCRH/_CR/_IMSC/_ICR`;
   flags `FR_BUSY/FR_RXFE/FR_TXFF`, `CR_UARTEN/CR_TXE/CR_RXE`,
   `LCRH_FEN/LCRH_WLEN8`
2. `uart_init()` — mask IRQs → clear pending (`ICR=0x7ff`) → `IBRD=13, FBRD=1`
   (115200 baud from the 24 MHz clock: 24e6/(16·115200)=13.02) → line control
   FIFO + 8-N-1 → enable UART/TX/RX
3. `uart_putc(char c)` — polls `FR.TXFF`; translates `\n` into `\r\n` on the wire
4. `uart_puts(const char *s)` — loops `uart_putc`
5. `uart_getc()` — polls `FR.RXFE`, returns low byte of `DR`

> **Commit 2:** `Add PL011 UART driver for early console output`

### 7. `include/lib.h`
Prototype: `kprintf(const char *fmt, ...)` with `format(printf, 1, 2)` attribute.

### 8. `lib/printf.c`
Helpers first (all `static`), public function last:
1. `put_str(const char *s)`
2. `put_ull(unsigned long long v, unsigned base, int uppercase)` — reverse-digit buffer
3. `put_ll(long long v)` — sign handling, delegates to `put_ull`
4. `kprintf(...)` — vararg loop; supports `%c %s %d %i %u %x %X %p %%` with
   optional `l`/`ll` length modifiers; no width/precision/float by design
   (kernel compiles with `-mgeneral-regs-only`)

### 9. `kernel/main.c`
1. Macro `BANNER "[OK] mobile_phone_os phase 0"`
2. `kmain(uint64_t el, uint64_t dtb)` — `uart_init()`, prints kernel name,
   entry EL, DTB pointer, banner; then sleeps forever in `wfi`

### 10. `Makefile`
Variables → pattern rules → targets, in order:
1. Toolchain vars: `CROSS ?= aarch64-linux-gnu-`, `CC/LD/OBJCOPY`
2. `BUILD=build`, `KERNEL=build/kernel.elf`, `IMG=build/kernel8.img`
3. `QEMU_ARGS := -M virt -cpu cortex-a53 -m 128M`
4. `CFLAGS`: `-Wall -Wextra -O2 -g -ffreestanding -fno-builtin
   -fno-stack-protector -fno-pic -nostdlib -march=armv8-a
   -mgeneral-regs-only`, include paths `-Iinclude -Idrivers -Ikernel`,
   dependency tracking `-MMD -MP`
5. `LDFLAGS := -T linker.ld -nostdlib` (linked with `$(LD)` directly)
6. Rules: `%.c → .o`, `%.S → .o` (mirrored under `build/`), ELF link,
   `objcopy -O binary` raw image
7. Targets: `all`, `run` (interactive, exit QEMU with `Ctrl-A X`),
   `test` (headless, 5 s timeout, greps `[OK]` in `build/serial.log`),
   `debug` (`-S -gdb tcp::1234` for `gdb-multiarch`), `clean`

> **Commit 3:** `Add kprintf, kernel C entry point and Makefile; phase 0 smoke test passes in QEMU`

## Build iterations (fixes applied)

| Iteration | Problem | Fix |
|-----------|---------|-----|
| 1 | `ld: unable to disambiguate: -nostartfiles` | removed it — GCC-only flag, we invoke `$(LD)` directly |
| 1 | `-Wformat=` warnings in main.c (`%llu` vs `uint64_t`, `%p` vs `uint64_t`) | explicit casts: `(unsigned long long)el`, `(void *)(uintptr_t)dtb` |
| 2 | clean build | one benign warning remains: `LOAD segment with RWX permissions` — expected until Phase 1 sets real page attributes in the linker script/MMU tables |

## Verification result

```
$ make test
SMOKE TEST: PASS

$ cat build/serial.log

mobile_phone_os kernel
boot: entered at EL1
boot: DTB pointer = 0x0
[OK] mobile_phone_os phase 0
```

Notes:
- QEMU `-kernel` with an ELF enters at **EL1** on the `virt` machine (no firmware),
  so no EL-drop code was needed yet (Phase 1 adds EL3/EL2 → EL1 transitions).
- `DTB pointer = 0x0` is expected: direct ELF boot leaves registers zeroed.
  Phase 1 locates the FDT via QEMU's fw_cfg or an explicit load address instead.

> **Commit 4:** `Add phase 0 implementation log`
