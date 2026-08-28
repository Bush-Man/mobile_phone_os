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
          -Iinclude -Idrivers -Ikernel \
          -MMD -MP

LDFLAGS := -T linker.ld -nostdlib

# ---- userspace (phase 5) ------------------------------------------------
# Static freestanding ELFs linked into the per-process user window.
# No FP/SIMD: the kernel does not save vector state across switches.
USER_CFLAGS := -Wall -Wextra -O2 -g \
               -ffreestanding -fno-builtin -nostdlib \
               -fno-pic -fno-pie \
               -march=armv8-a -mgeneral-regs-only

SRCS_C := $(wildcard kernel/*.c drivers/*.c lib/*.c mm/*.c fs/*.c \
           net/*.c arch/aarch64/*.c)
SRCS_S := $(wildcard arch/aarch64/*.S)
OBJS   := $(addprefix $(BUILD)/,$(SRCS_C:.c=.o) $(SRCS_S:.S=.o))
OBJS   += $(BUILD)/fdt_blob.o
DEPS   := $(filter-out $(BUILD)/fdt_blob.o,$(OBJS:.o=.d))

.PHONY: all run test debug dtb clean userspace

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

$(BUILD)/arch/aarch64/builtin_imgs.o: userspace/hello userspace/ipcdemo \
                                       userspace/evreader

dtb:
	$(QEMU) -M virt -cpu cortex-a53 -m 128M -display none \
	    -machine dumpdtb=platform/qemu-virt.dtb

run: all
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL)

test: all
	@python3 tests/serial_harness.py "$(QEMU)" "$(QEMU_ARGS)" \
	    $(KERNEL) $(BUILD)/serial.log 10

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
