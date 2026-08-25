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

# Milestone 24: separate flags for ring-3 userspace C code (kernel/user/
# rt/, and any *.c program built on it) -- NOT the kernel's own CFLAGS.
# -mcmodel=large, not =kernel: process-private code is architecturally
# constrained to live at PML4 index >= 1 (index 0 is committed to the
# shared identity map, ADR 0009), i.e. VA >= 0x8000000000 -- far outside
# what the small/kernel code models' 32-bit-displacement addressing can
# reach, the same reason every hand-written .asm user program already
# needed `default rel` (RIP-relative addressing). -mcmodel=large forces
# full 64-bit immediate addressing instead, correct regardless of link
# address. No -mno-red-zone: unlike kernel code, ring-3 code never runs
# an interrupt/exception handler ON ITS OWN stack (every trap switches
# to the kernel's own TSS.RSP0 stack, ADR 0007) -- the classic
# red-zone-clobbered-by-an-interrupt hazard that rule exists for on the
# kernel side doesn't apply here, so ordinary SysV ABI leaf-function
# red-zone usage is safe. No FP/SSE, matching the kernel: the scheduler
# doesn't save/restore FPU/SSE state for ANY task yet (CLAUDE.md: no FP/
# SSE before that milestone exists), kernel or user.
USER_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mcmodel=large \
               -mno-sse -mno-sse2 -mno-mmx -msoft-float \
               -Wall -Wextra -Werror -c

LDFLAGS := -ffreestanding -nostdlib -T boot/linker.ld

ASFLAGS := -f elf64

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/iso

C_SOURCES := kernel/kernel.c kernel/panic.c kernel/shell.c kernel/drivers/serial.c kernel/drivers/framebuffer.c kernel/drivers/fbconsole.c \
             kernel/drivers/cursor.c kernel/drivers/console.c \
             kernel/drivers/pic.c kernel/drivers/pit.c kernel/drivers/keyboard.c kernel/drivers/mouse.c kernel/drivers/pci.c kernel/drivers/rtc.c \
             libk/fmt.c libk/heap_alloc.c libk/ring_buffer.c libk/elf.c \
             kernel/arch/x86_64/gdt.c kernel/arch/x86_64/idt.c kernel/arch/x86_64/exceptions.c kernel/arch/x86_64/irq_dispatch.c \
             kernel/arch/x86_64/tss.c kernel/arch/x86_64/syscall.c kernel/arch/x86_64/reboot.c kernel/arch/x86_64/multiboot2.c \
             kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/heap.c kernel/mm/elf_loader.c \
             kernel/ipc/msgqueue.c kernel/ipc/shm.c \
             kernel/sched/task.c kernel/sched/scheduler.c
ASM_SOURCES := kernel/arch/x86_64/boot.asm kernel/arch/x86_64/gdt_flush.asm kernel/arch/x86_64/isr.asm kernel/arch/x86_64/irq.asm \
               kernel/arch/x86_64/syscall_entry.asm kernel/user/embed/user_elf_blob.asm kernel/user/embed/fork_demo_blob.asm \
               kernel/user/embed/exec_demo_blob.asm kernel/user/embed/exec_target_blob.asm \
               kernel/user/embed/ipc_sender_blob.asm kernel/user/embed/ipc_receiver_blob.asm \
               kernel/user/embed/display_server_blob.asm kernel/user/embed/display_client_blob.asm

C_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
OS_ISO := $(BUILD_DIR)/os.iso

# Milestone 17/18: embedded userspace ELF64 executables (kernel/user/
# *.asm + user.ld) -- completely separate links (plain `x86_64-elf-ld
# -T`, no kernel CFLAGS/LDFLAGS: ring-3 user code, not kernel code) that
# must each exist BEFORE the matching kernel/user/embed/*_blob.asm is
# assembled, since its `incbin` reads the file's raw bytes directly.
# Milestone 24: blob-embedding files moved from kernel/sched/ (an
# artifact of when each was first added, not a deliberate placement) to
# kernel/user/embed/ -- "embed this program into the kernel image" is a
# userspace-packaging concern, not a scheduler one, and this scales much
# better once the GUI arc's window server + multiple apps all need one.
USER_ELF := $(BUILD_DIR)/kernel/user/hello.elf
USER_ELF_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/user_elf_blob.o
FORK_DEMO_ELF := $(BUILD_DIR)/kernel/user/fork_demo.elf
FORK_DEMO_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/fork_demo_blob.o
EXEC_DEMO_ELF := $(BUILD_DIR)/kernel/user/exec_demo.elf
EXEC_DEMO_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/exec_demo_blob.o
EXEC_TARGET_ELF := $(BUILD_DIR)/kernel/user/exec_target.elf
EXEC_TARGET_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/exec_target_blob.o
IPC_SENDER_ELF := $(BUILD_DIR)/kernel/user/ipc_sender.elf
IPC_SENDER_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/ipc_sender_blob.o
IPC_RECEIVER_ELF := $(BUILD_DIR)/kernel/user/ipc_receiver.elf
IPC_RECEIVER_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/ipc_receiver_blob.o
DISPLAY_SERVER_ELF := $(BUILD_DIR)/kernel/user/display_server.elf
DISPLAY_SERVER_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/display_server_blob.o
DISPLAY_CLIENT_ELF := $(BUILD_DIR)/kernel/user/display_client.elf
DISPLAY_CLIENT_BLOB_OBJ := $(BUILD_DIR)/kernel/user/embed/display_client_blob.o

# Milestone 24: the minimal userspace C runtime (Desktop.md) -- crt0
# (asm, built via the ordinary $(BUILD_DIR)/%.o: %.asm rule below) plus
# a small freestanding C library (built via USER_CFLAGS, NOT the
# kernel's CFLAGS -- see USER_C_SOURCES' static pattern rule below).
# Every C-based user program links against all of these.
USER_RT_ASM_OBJECTS := $(BUILD_DIR)/kernel/user/rt/crt0.o
USER_RT_C_SOURCES := kernel/user/rt/syscall.c kernel/user/rt/string.c
USER_RT_C_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(USER_RT_C_SOURCES))
USER_RT_OBJECTS := $(USER_RT_ASM_OBJECTS) $(USER_RT_C_OBJECTS)

# Every C-based user PROGRAM (as opposed to runtime-library source) --
# just hello.c for now, kernel/user/hello.asm's own rewrite proving the
# runtime works (Desktop.md); the GUI arc's window server/apps join
# this list as their own milestones land.
USER_C_SOURCES := $(USER_RT_C_SOURCES) kernel/user/hello.c kernel/user/ipc_sender.c kernel/user/ipc_receiver.c \
                   kernel/user/display_server.c kernel/user/display_client.c
USER_C_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(USER_C_SOURCES))

.PHONY: all run debug clean check-mb2

all: $(OS_ISO)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

# Milestone 24: a STATIC pattern rule (bound to this exact target list,
# $(USER_C_OBJECTS)) rather than a second generic `%.o: %.c` rule --
# GNU Make static pattern rules take unambiguous precedence for their
# own listed targets over a separately-defined implicit pattern rule
# for the same targets, so this doesn't fight with the kernel-CFLAGS
# rule immediately above for any file outside kernel/user/. Kept as an
# explicit target list rather than relying on implicit-rule stem-length
# precedence, which would work here too but is a much less legible way
# to express "these specific files are compiled differently."
$(USER_C_OBJECTS): $(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $< -o $@

$(USER_ELF_BLOB_OBJ): $(USER_ELF)
$(FORK_DEMO_BLOB_OBJ): $(FORK_DEMO_ELF)
$(EXEC_DEMO_BLOB_OBJ): $(EXEC_DEMO_ELF)
$(EXEC_TARGET_BLOB_OBJ): $(EXEC_TARGET_ELF)
$(IPC_SENDER_BLOB_OBJ): $(IPC_SENDER_ELF)
$(IPC_RECEIVER_BLOB_OBJ): $(IPC_RECEIVER_ELF)
$(DISPLAY_SERVER_BLOB_OBJ): $(DISPLAY_SERVER_ELF)
$(DISPLAY_CLIENT_BLOB_OBJ): $(DISPLAY_CLIENT_ELF)

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_ELF): $(BUILD_DIR)/kernel/user/hello.o $(USER_RT_OBJECTS) kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(USER_RT_OBJECTS) $(BUILD_DIR)/kernel/user/hello.o

$(FORK_DEMO_ELF): $(BUILD_DIR)/kernel/user/fork_demo.o kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(BUILD_DIR)/kernel/user/fork_demo.o

$(EXEC_DEMO_ELF): $(BUILD_DIR)/kernel/user/exec_demo.o kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(BUILD_DIR)/kernel/user/exec_demo.o

$(EXEC_TARGET_ELF): $(BUILD_DIR)/kernel/user/exec_target.o kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(BUILD_DIR)/kernel/user/exec_target.o

$(IPC_SENDER_ELF): $(BUILD_DIR)/kernel/user/ipc_sender.o $(USER_RT_OBJECTS) kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(USER_RT_OBJECTS) $(BUILD_DIR)/kernel/user/ipc_sender.o

$(IPC_RECEIVER_ELF): $(BUILD_DIR)/kernel/user/ipc_receiver.o $(USER_RT_OBJECTS) kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(USER_RT_OBJECTS) $(BUILD_DIR)/kernel/user/ipc_receiver.o

$(DISPLAY_SERVER_ELF): $(BUILD_DIR)/kernel/user/display_server.o $(USER_RT_OBJECTS) kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(USER_RT_OBJECTS) $(BUILD_DIR)/kernel/user/display_server.o

$(DISPLAY_CLIENT_ELF): $(BUILD_DIR)/kernel/user/display_client.o $(USER_RT_OBJECTS) kernel/user/user.ld
	@mkdir -p $(dir $@)
	$(LD) -T kernel/user/user.ld --nostdlib -o $@ $(USER_RT_OBJECTS) $(BUILD_DIR)/kernel/user/display_client.o

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
# `debug` open a real GTK window showing the graphics framebuffer
# console (kernel/drivers/fbconsole.c, Milestone 23) -- serial still
# goes to this terminal via -serial stdio. Every automated QEMU
# smoke test (tests/qemu/*.sh) invokes qemu-system-x86_64 directly with
# its own -display none, so this doesn't touch CI/test behavior.
run: $(OS_ISO)
	$(QEMU) -cdrom $(OS_ISO) -serial stdio -no-reboot -no-shutdown -display gtk

debug: $(OS_ISO)
	$(QEMU) -cdrom $(OS_ISO) -serial stdio -no-reboot -no-shutdown -display gtk -s -S &
	$(GDB) $(KERNEL_ELF) -ex "target remote :1234"

clean:
	rm -rf $(BUILD_DIR)
