# Mobile Phone OS - AArch64 build
CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf
IMG     := $(BUILD)/kernel8.img

QEMU       ?= qemu-system-aarch64
QEMU_ARGS  := -M virt -cpu cortex-a53 -m 128M

CFLAGS := -Wall -Wextra -O2 -g \
          -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-pic -nostdlib \
          -march=armv8-a -mgeneral-regs-only \
          -Iinclude -Idrivers -Ikernel \
          -MMD -MP

LDFLAGS := -T linker.ld -nostdlib

SRCS_C := $(wildcard kernel/*.c drivers/*.c lib/*.c)
SRCS_S := $(wildcard arch/aarch64/*.S)
OBJS   := $(addprefix $(BUILD)/,$(SRCS_C:.c=.o) $(SRCS_S:.S=.o))
DEPS   := $(OBJS:.o=.d)

.PHONY: all run test debug clean

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

run: all
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL)

test: all
	@rm -f $(BUILD)/serial.log
	@timeout 5 $(QEMU) $(QEMU_ARGS) -display none -monitor none \
	    -serial file:$(BUILD)/serial.log -kernel $(KERNEL) || true
	@grep -q "\[OK\] mobile_phone_os phase 0" $(BUILD)/serial.log && \
	    echo "SMOKE TEST: PASS" || \
	    { echo "SMOKE TEST: FAIL"; cat $(BUILD)/serial.log 2>/dev/null; exit 1; }

debug: all
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL) -S -gdb tcp::1234

clean:
	rm -rf $(BUILD)

-include $(DEPS)
