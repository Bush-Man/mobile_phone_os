# Mobile Phone OS - AArch64 build
CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf
IMG     := $(BUILD)/kernel8.img

QEMU       ?= qemu-system-aarch64
QEMU_ARGS  := -M virt -cpu cortex-a53 -m 128M -smp 2

CFLAGS := -Wall -Wextra -O2 -g \
          -ffreestanding -fno-builtin \
          -fstack-protector-strong -mstack-protector-guard=global \
          -fno-pic -nostdlib \
          -march=armv8-a -mgeneral-regs-only \
          -mno-outline-atomics \
          -Iinclude -Idrivers -Ikernel \
          -MMD -MP

LDFLAGS := -T linker.ld -nostdlib

# ---- userspace (phase 5) ------------------------------------------------
# Static freestanding ELFs linked into the per-process user window.
# No FP/SIMD: the kernel does not save vector state across switches.
# -mno-outline-atomics: libc's pthreads use __atomic builtins, and
# the default outline path calls libgcc helpers (__aarch64_swp1_acq)
# that -nostdlib cannot link; inline LDXR/STXR loops need nothing.
USER_CFLAGS := -Wall -Wextra -O2 -g \
               -ffreestanding -fno-builtin -nostdlib \
               -fno-pic -fno-pie \
               -march=armv8-a -mgeneral-regs-only \
               -mno-outline-atomics

SRCS_C := $(wildcard kernel/*.c drivers/*.c lib/*.c mm/*.c fs/*.c \
           net/*.c arch/aarch64/*.c)
SRCS_S := $(wildcard arch/aarch64/*.S)
OBJS   := $(addprefix $(BUILD)/,$(SRCS_C:.c=.o) $(SRCS_S:.S=.o))
OBJS   += $(BUILD)/fdt_blob.o
DEPS   := $(filter-out $(BUILD)/fdt_blob.o,$(OBJS:.o=.d))

.PHONY: all run test debug dtb clean userspace release

all: $(KERNEL) $(IMG)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(IMG): $(KERNEL)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/fdt_blob.o: platform/qemu-virt.dtb
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	    --rename-section .data=.rodata.fdt,alloc,load,readonly,data,contents $< $@

# ---- userspace (phase 5) ------------------------------------------------
# builtin_imgs.S embeds userspace/hello and userspace/ipcdemo verbatim
# (ELF headers and all), so both must exist before that object is
# assembled.

userspace/hello: userspace/hello.c userspace/hello.ld
	$(CC) $(USER_CFLAGS) -no-pie -T userspace/hello.ld \
	    -Wl,--build-id=none -o $@ $<

userspace/ipcdemo: userspace/ipcdemo.c userspace/ipcdemo.ld
	$(CC) $(USER_CFLAGS) -no-pie -T userspace/ipcdemo.ld \
	    -Wl,--build-id=none -o $@ $<

userspace/evreader: userspace/evreader.c userspace/evreader.ld
	$(CC) $(USER_CFLAGS) -no-pie -T userspace/evreader.ld \
	    -Wl,--build-id=none -o $@ $<

userspace/netcli: userspace/netcli.c userspace/netcli.ld
	$(CC) $(USER_CFLAGS) -no-pie -T userspace/netcli.ld \
	    -Wl,--build-id=none -o $@ $<

# ---- phase 14: libc-linked userspace foundation ---------------------------
# Every program below links the libc objects (crt0.o FIRST, so _start
# owns the entry point) plus its own main file. One shared linker
# script: userspace/libc/user.ld.

LIBC_OBJS := $(BUILD)/libc_crt0.o $(BUILD)/libc_string.o \
             $(BUILD)/libc_stdio.o $(BUILD)/libc_malloc.o \
             $(BUILD)/libc_unistd.o $(BUILD)/libc_pthread.o

$(BUILD)/libc_%.o: userspace/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include -c $< -o $@

userspace/init: userspace/init.c $(LIBC_OBJS) userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS)

userspace/sh: userspace/sh.c $(LIBC_OBJS) userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS)

userspace/batteryd: userspace/batteryd.c $(LIBC_OBJS) \
                    userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS)

userspace/udevd: userspace/udevd.c $(LIBC_OBJS) userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS)

userspace/timed: userspace/timed.c $(LIBC_OBJS) userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS)

userspace/libctest: userspace/libctest.c $(LIBC_OBJS) \
                    userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS)

userspace/crasher: userspace/crasher.c $(LIBC_OBJS) \
                   userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS)

# ---- phase 15: compositor + phone apps --------------------------------------
# UI programs link the libc objects PLUS the phase-15 toolkit
# (surface gfx, widgets/keyboard, client library). -Iinclude is
# needed for ui_layout.h, the chrome-geometry contract shared with
# the kernel selftest.

UI_LIB_OBJS := $(BUILD)/ui_gfx.o $(BUILD)/ui_widgets.o \
               $(BUILD)/ui_client.o

# the stem excludes the ui_ prefix (build/ui_gfx.o <- ui_gfx.c)
$(BUILD)/ui_%.o: userspace/ui/ui_%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include -Iinclude \
	    -c $< -o $@

UI_PROGS := compositor dialer msgs contacts clock calc settings \
            uitest

# Static pattern rule (targets : target-pattern : prereq-patterns):
# as an ordinary rule the literal prerequisite "userspace/%.c" has
# no rule and make stops before building any UI program.
# -Iuserspace/ui: the apps live in userspace/ and quote-include
# "ui.h" from userspace/ui/ (the toolkit objects compile from
# inside that directory, so only the program rule needs the flag).
$(UI_PROGS:%=userspace/%): userspace/%: userspace/%.c $(LIBC_OBJS) \
                           $(UI_LIB_OBJS) userspace/libc/user.ld
	$(CC) $(USER_CFLAGS) -Iuserspace/libc/include -Iinclude \
	    -Iuserspace/ui \
	    -T userspace/libc/user.ld -Wl,--build-id=none -o $@ $< \
	    $(LIBC_OBJS) $(UI_LIB_OBJS)

$(BUILD)/arch/aarch64/builtin_imgs.o: userspace/hello userspace/ipcdemo \
                                       userspace/evreader userspace/netcli \
                                       userspace/init userspace/sh \
                                       userspace/batteryd userspace/udevd \
                                       userspace/timed userspace/libctest \
                                       userspace/crasher \
                                       $(UI_PROGS:%=userspace/%)

dtb:
	$(QEMU) -M virt -cpu cortex-a53 -m 128M -display none \
	    -machine dumpdtb=platform/qemu-virt.dtb

run: all
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL)

test: all
	@python3 tests/serial_harness.py "$(QEMU)" "$(QEMU_ARGS)" \
	    $(KERNEL) $(BUILD)/serial.log 16

# ---- phase 16: release image builder (item 89) ------------------------------
# Assembles the reproducible release bundle: the boot image, the ELF
# with symbols, the docs set, and a manifest pinning the exact source
# (git hash) with sha256 sums. Host-side only; see docs/RELEASE.md.
# Override BOARD=pinephone to select a board config in the script.
release: all
	@bash scripts/build-release.sh "$(BUILD)" "$(GIT_HASH)"

GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

# ---- phase 9: graphics + input demo targets -------------------------------
# virtio-gpu renders into an off-screen host surface by default;
# override DISPLAYARGS (e.g. make run-display DISPLAYARGS="-display gtk")
# to see the test pattern live on your desktop.
DISPLAYARGS ?= -display none
INPUTDEVS   := -device virtio-gpu-device \
               -device virtio-tablet-device \
               -device virtio-keyboard-device

run-display: all disk.img
	$(QEMU) $(QEMU_ARGS) $(INPUTDEVS) -kernel $(KERNEL) \
	    $(DISPLAYARGS) -serial stdio \
	    -netdev user,id=p11n0 \
	    -device virtio-net-device,netdev=p11n0 \
	    -drive if=none,file=disk.img,format=raw,id=p6hd \
	    -device virtio-blk-device,drive=p6hd

debug: all
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL) -S -gdb tcp::1234

clean:
	rm -rf $(BUILD)

# ---- phase 6: virtio disk/net demo ---------------------------------------
# Scratch disk for the block-layer selftest; created once, then
# reused (the kernel writes its own MBR onto a blank image).

disk.img:
	dd if=/dev/zero of=$@ bs=1M count=64

run-disk: all disk.img
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL) \
	    -drive if=none,file=disk.img,format=raw,id=p6hd \
	    -device virtio-blk-device,drive=p6hd \
	    -netdev user,id=p6n0 \
	    -device virtio-net-device,netdev=p6n0

-include $(DEPS)
