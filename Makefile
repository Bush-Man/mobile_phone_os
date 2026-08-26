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

SRCS_C := $(wildcard kernel/*.c drivers/*.c lib/*.c mm/*.c \
           arch/aarch64/*.c)
SRCS_S := $(wildcard arch/aarch64/*.S)
OBJS   := $(addprefix $(BUILD)/,$(SRCS_C:.c=.o) $(SRCS_S:.S=.o))
OBJS   += $(BUILD)/fdt_blob.o
DEPS   := $(filter-out $(BUILD)/fdt_blob.o,$(OBJS:.o=.d))

.PHONY: all run test debug dtb clean

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

dtb:
	$(QEMU) -M virt -cpu cortex-a53 -m 128M -display none \
	    -machine dumpdtb=platform/qemu-virt.dtb

run: all
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL)

test: all
	@python3 tests/serial_harness.py "$(QEMU)" "$(QEMU_ARGS)" \
	    $(KERNEL) $(BUILD)/serial.log 4

debug: all
	$(QEMU) $(QEMU_ARGS) -nographic -kernel $(KERNEL) -S -gdb tcp::1234

clean:
	rm -rf $(BUILD)

-include $(DEPS)
