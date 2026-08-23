# Milestone 1: Boot -> "hello kernel" via serial.
# See CLAUDE.md "Build/run/debug" for the invariant command shapes.

CC := x86_64-elf-gcc
AS := nasm
GRUB_MKRESCUE := grub-mkrescue
GRUB_FILE := grub-file
QEMU := qemu-system-x86_64
GDB := gdb

CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
          -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -msoft-float \
          -Wall -Wextra -Werror -c

LDFLAGS := -ffreestanding -nostdlib -T boot/linker.ld

ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/iso

C_SOURCES := kernel/kernel.c kernel/drivers/serial.c libk/fmt.c \
             kernel/arch/x86_64/gdt.c kernel/arch/x86_64/idt.c kernel/arch/x86_64/exceptions.c
ASM_SOURCES := kernel/arch/x86_64/boot.asm kernel/arch/x86_64/gdt_flush.asm kernel/arch/x86_64/isr.asm

C_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
OS_ISO := $(BUILD_DIR)/os.iso

.PHONY: all run debug clean check-mb2

all: $(OS_ISO)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_ELF): $(OBJECTS) boot/linker.ld
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ -lgcc

check-mb2: $(KERNEL_ELF)
	$(GRUB_FILE) --is-x86-multiboot2 $(KERNEL_ELF) && \
		echo "[OK] $(KERNEL_ELF) has a valid multiboot2 header"

$(OS_ISO): $(KERNEL_ELF) boot/grub.cfg check-mb2
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

run: $(OS_ISO)
	$(QEMU) -cdrom $(OS_ISO) -serial stdio -no-reboot -no-shutdown -display none

debug: $(OS_ISO)
	$(QEMU) -cdrom $(OS_ISO) -serial stdio -no-reboot -no-shutdown -display none -s -S &
	$(GDB) $(KERNEL_ELF) -ex "target remote :1234"

clean:
	rm -rf $(BUILD_DIR)
