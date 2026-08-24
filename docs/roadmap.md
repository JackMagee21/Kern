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

## 2. GDT + IDT + exception handlers — DONE
**Proves:** visibility into faults — everything after this depends on being
able to see *why* something broke instead of triple-faulting silently.
**Deliverables:**
- `kernel/arch/x86_64/gdt.c/.h`, `gdt_flush.asm`: C-managed 3-entry flat
  GDT (null/kernel-code/kernel-data), replacing boot.asm's throwaway one.
- `kernel/arch/x86_64/idt.c/.h`: 256-entry IDT, vectors 0-31 (CPU
  exceptions) populated as 64-bit interrupt gates; 32+ left not-present
  until Milestone 5 (IRQs).
- `kernel/arch/x86_64/isr.asm`: one stub per exception vector (0-31),
  normalizing to a uniform trap-frame shape and handling the long-mode
  stack-alignment gotcha explicitly (see ADR 0002).
- `kernel/arch/x86_64/exceptions.c`, `trap_frame.h`: `isr_handler` dumps
  vector/name/error-code/CR2(on #PF)/all registers to serial, then halts
  (no recovery path exists yet — every exception is fatal).
- `libk/fmt.c/.h`: `u64_to_hex`, host-tested (`tests/host/test_fmt.c`).
- `kernel/kernel.c`: calls `gdt_init()`/`idt_init()`, then deliberately
  triggers `int3` as a self-test that the fault-dump path works end to end.
**Verification:** `make run` boots the real ISO and prints `[OK] hello
kernel` → `[OK] gdt/idt installed` → the `int3` self-test's fault dump
(`#BP Breakpoint`, vector `0x3`, `cs=0x8`/`ss=0x10` matching the GDT
exactly, sane `rip`/`rsp`/all GPRs). `tests/qemu/test_boot_serial.sh` and
`tests/qemu/test_idt_selftest.sh` both pass. See ADR 0002 for the full
verification trail (object/link-level checks done before the toolchain
existed, plus what the live boot then confirmed).
**Design record:** `docs/adr/0002-gdt-idt-exception-handling.md`.
**Known limitation (accepted for this milestone only):** no TSS, no IST
stacks — a fault while the kernel stack itself is corrupt (e.g. stack
overflow) will double-fault onto the same bad stack rather than a
dedicated safe one. Revisit if/when that's actually observed, or
alongside the scheduler once interrupts can land on arbitrary thread
stacks.

## 3. Physical frame allocator — DONE
**Proves:** the kernel can actually discover and hand out real physical
memory, one 4KiB frame at a time — everything Milestone 4 builds (page
tables, kernel heap) needs frames to come from somewhere real.
**Deliverables:**
- `kernel/arch/x86_64/multiboot2.h`: Multiboot2 info-structure tag/mmap-
  entry layout, verified against GRUB's own header (see ADR 0003).
- `kernel/mm/pmm.c/.h`: bitmap frame allocator (128KiB bitmap, 4GiB
  tracking limit), parses the real memory map via `mbi_addr` (received
  since Milestone 1, unused until now), default-deny then carves out
  reserved ranges (frame 0, kernel image, multiboot info structure).
- `boot/linker.ld`, `kernel/arch/x86_64/boot.asm`: `__bss_start`/
  `__bss_end` + an explicit `.bss` zero before `call kernel_main` — a
  gap that predated this milestone (see ADR 0003) but only became
  load-bearing once a subsystem's correctness actually depended on it.
- `kernel/kernel.c`: calls `pmm_init(mbi_addr)`, prints the free/total
  frame counts, then an alloc→alloc→free→realloc self-test.
**Verification:** `make run` boots the real ISO and prints `[OK] pmm
initialized, free frames: 0x7eaf / total: 0x100000` (126.7MiB free,
4GiB tracked — both sanity-checkable, not just present) then `[OK] pmm
self-test passed (alloc/free/reuse)`. `tests/qemu/test_pmm_selftest.sh`
(new) plus the Milestone 1/2 smoke tests all re-verified passing after
the `.bss` fix. See ADR 0003 for the full trail.
**Design record:** `docs/adr/0003-physical-frame-allocator.md`.
**Known limitation (accepted for this milestone only):** frames above
4GiB physical are never tracked/allocatable (fixed-size bitmap, no heap
yet to size one dynamically) — revisit only if a real target's RAM ever
approaches that limit.

## 4. Paging/VMM + kernel heap
## 5. PIT/APIC timer + IRQ handling
## 6. Preemptive scheduler + context switch (single CPU)
## 7. Userspace: ring 3, syscalls, process model
## 8. Later: FS, drivers, SMP (sequence TBD from what's learned above)

Milestones 4–8 are intentionally left as one-line placeholders here — full
breakdown (deliverables/acceptance criteria/estimates/risks) gets written
up when that milestone actually starts, not in advance, to avoid designing
against assumptions already-implemented milestones might overturn.
