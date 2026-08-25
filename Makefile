# Milestone 1: Boot -> "hello kernel" via serial.
# See CLAUDE.md "Build/run/debug" for the invariant command shapes.

CC := x86_64-elf-gcc
AS := nasm
LD := x86_64-elf-ld
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

C_SOURCES := kernel/kernel.c kernel/panic.c kernel/shell.c kernel/drivers/serial.c kernel/drivers/vga.c kernel/drivers/console.c \
             kernel/drivers/pic.c kernel/drivers/pit.c kernel/drivers/keyboard.c kernel/drivers/mouse.c kernel/drivers/pci.c kernel/drivers/rtc.c \
             libk/fmt.c libk/heap_alloc.c libk/ring_buffer.c libk/elf.c \
             kernel/arch/x86_64/gdt.c kernel/arch/x86_64/idt.c kernel/arch/x86_64/exceptions.c kernel/arch/x86_64/irq_dispatch.c \
             kernel/arch/x86_64/tss.c kernel/arch/x86_64/syscall.c kernel/arch/x86_64/reboot.c \
             kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/heap.c kernel/mm/elf_loader.c \
             kernel/sched/task.c kernel/sched/scheduler.c
ASM_SOURCES := kernel/arch/x86_64/boot.asm kernel/arch/x86_64/gdt_flush.asm kernel/arch/x86_64/isr.asm kernel/arch/x86_64/irq.asm \
               kernel/arch/x86_64/syscall_entry.asm kernel/sched/user_elf_blob.asm kernel/sched/fork_demo_blob.asm \
               kernel/sched/exec_demo_blob.asm kernel/sched/exec_target_blob.asm

C_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
OS_ISO := $(BUILD_DIR)/os.iso

# Milestone 17/18: embedded userspace ELF64 executables (kernel/user/
# *.asm + user.ld) -- completely separate links (plain `x86_64-elf-ld
# -T`, no kernel CFLAGS/LDFLAGS: ring-3 user code, not kernel code) that
# must each exist BEFORE the matching kernel/sched/*_blob.asm is
# assembled, since its `incbin` reads the file's raw bytes directly.
USER_ELF := $(BUILD_DIR)/kernel/user/hello.elf
USER_ELF_BLOB_OBJ := $(BUILD_DIR)/kernel/sched/user_elf_blob.o
FORK_DEMO_ELF := $(BUILD_DIR)/kernel/user/fork_demo.elf
FORK_DEMO_BLOB_OBJ := $(BUILD_DIR)/kernel/sched/fork_demo_blob.o
EXEC_DEMO_ELF := $(BUILD_DIR)/kernel/user/exec_demo.elf
EXEC_DEMO_BLOB_OBJ := $(BUILD_DIR)/kernel/sched/exec_demo_blob.o
EXEC_TARGET_ELF := $(BUILD_DIR)/kernel/user/exec_target.elf
EXEC_TARGET_BLOB_OBJ := $(BUILD_DIR)/kernel/sched/exec_target_blob.o

.PHONY: all run debug clean check-mb2

all: $(OS_ISO)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(USER_ELF_BLOB_OBJ): $(USER_ELF)
$(FORK_DEMO_BLOB_OBJ): $(FORK_DEMO_ELF)
$(EXEC_DEMO_BLOB_OBJ): $(EXEC_DEMO_ELF)
$(EXEC_TARGET_BLOB_OBJ): $(EXEC_TARGET_ELF)

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_ELF): $(BUILD_DIR)/kernel/user/hello.o kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(BUILD_DIR)/kernel/user/hello.o

$(FORK_DEMO_ELF): $(BUILD_DIR)/kernel/user/fork_demo.o kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(BUILD_DIR)/kernel/user/fork_demo.o

$(EXEC_DEMO_ELF): $(BUILD_DIR)/kernel/user/exec_demo.o kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(BUILD_DIR)/kernel/user/exec_demo.o

$(EXEC_TARGET_ELF): $(BUILD_DIR)/kernel/user/exec_target.o kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(BUILD_DIR)/kernel/user/exec_target.o

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

# WSLg confirmed working on this machine (DISPLAY/WAYLAND_DISPLAY set,
# /tmp/.X11-unix/X0 present) and qemu-ui-gtk is installed, so `run`/
# `debug` open a real GTK window showing the VGA console -- serial
# still goes to this terminal via -serial stdio. Every automated QEMU
# smoke test (tests/qemu/*.sh) invokes qemu-system-x86_64 directly with
# its own -display none, so this doesn't touch CI/test behavior.
run: $(OS_ISO)
	$(QEMU) -cdrom $(OS_ISO) -serial stdio -no-reboot -no-shutdown -display gtk

debug: $(OS_ISO)
	$(QEMU) -cdrom $(OS_ISO) -serial stdio -no-reboot -no-shutdown -display gtk -s -S &
	$(GDB) $(KERNEL_ELF) -ex "target remote :1234"

clean:
	rm -rf $(BUILD_DIR)
