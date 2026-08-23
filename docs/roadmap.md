# Roadmap

Sequenced hardest-unknown / highest-leverage first, per CLAUDE.md. Each
milestone's deliverable must be proven by an actual QEMU smoke test in
`/tests/qemu`, not just "it compiled."

## 1. Boot → "hello kernel" via serial — DONE (this change)
**Proves:** toolchain + boot chain (cross-compiler, NASM, GRUB2/Multiboot2,
higher-half long-mode transition) all actually work together.
**Deliverables:**
- `boot/linker.ld`, `boot/grub.cfg`
- `kernel/arch/x86_64/boot.asm`: Multiboot2 header, 32-bit entry, CPUID/
  long-mode checks, boot page tables, GDT64, transition to long mode,
  jump to higher-half `kernel_main`.
- `kernel/drivers/serial.c/.h`: polling 16550 UART driver (COM1).
- `kernel/kernel.c`: `kernel_main`, prints `[OK] hello kernel`, halts.
- `libk/io.h`: `inb`/`outb`/`io_wait`.
**Verification:** `make check-mb2` (grub-file), `tests/qemu/test_boot_serial.sh`
(boots headless, greps serial output for the marker).
**Design record:** `docs/adr/0001-boot-protocol-and-long-mode-entry.md`.
**Known limitation (accepted for this milestone only):** boot page tables
only map the low 8MiB of physical memory (2MiB pages, no 4KiB level) —
replaced by a real VMM in Milestone 4.

## 2. GDT + IDT + exception handlers
**Proves:** visibility into faults — everything after this depends on being
able to see *why* something broke instead of triple-faulting silently.
**Scope (not yet detailed further — flag before expanding):** a proper
C-managed GDT (replacing the ad hoc `gdt64` in boot.asm), IDT with all 32
CPU exception vectors wired to handlers that dump full register/fault
state to serial per CLAUDE.md's panic rule, ISR/IRQ entry stubs with
verified stack alignment and trap-frame field order (see CLAUDE.md's
"Known x86_64 gotchas").

## 3. Physical frame allocator
## 4. Paging/VMM + kernel heap
## 5. PIT/APIC timer + IRQ handling
## 6. Preemptive scheduler + context switch (single CPU)
## 7. Userspace: ring 3, syscalls, process model
## 8. Later: FS, drivers, SMP (sequence TBD from what's learned above)

Milestones 3–8 are intentionally left as one-line placeholders here — full
breakdown (deliverables/acceptance criteria/estimates/risks) gets written
up when that milestone actually starts, not in advance, to avoid designing
against assumptions Milestones 1–2 might overturn.
