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

## 4. Paging/VMM + kernel heap — DONE
**Proves:** the kernel can turn a physical frame into usable memory at an
arbitrary virtual address, and allocate/free heap memory dynamically —
removes the "no malloc/free" constraint every prior milestone worked
around with static arrays.
**Deliverables:**
- `kernel/mm/vmm.c/.h`: 4-level page-table walker/mapper
  (`vmm_map_page`/`vmm_unmap_page`), extending boot.asm's live PML4
  (reused via `CR3`, never reloaded) with a new dedicated 1GiB region
  for the heap rather than touching the kernel image's own boot-time
  2MiB mapping. `invlpg` after every map/unmap.
- `libk/heap_alloc.c/.h`: first-fit free-list allocator with splitting/
  coalescing, pure hardware-free logic, host-tested
  (`tests/host/test_heap_alloc.c`, ASan/UBSan, 5 checks).
- `kernel/mm/heap.c/.h`: `kmalloc`/`kfree`, backed by a 1MiB region
  `heap_init()` eagerly maps via the VMM.
- `kernel/panic.c/.h`: shared `panic()`, factored out of `kernel_main`
  now that `vmm.c`/`heap.c` need it too (three real call sites, not
  speculative infrastructure).
- `kernel/kernel.c`: calls `heap_init()`, then an alloc→write→verify→
  free→reuse self-test.
**Verification:** `tests/host/test_heap_alloc.c` passes (5 checks,
ASan/UBSan). `make run` boots and prints `[OK] kernel heap initialized`
→ `[OK] heap self-test passed (alloc/write/verify/free/reuse)`.
`tests/qemu/test_heap_selftest.sh` (new) plus all three earlier
milestones' smoke tests re-verified passing. See ADR 0004 for the full
trail, including the identity-window invariant `vmm.c` checks at
runtime rather than assumes.
**Design record:** `docs/adr/0004-vmm-and-kernel-heap.md`.
**Known limitation (accepted for this milestone only):** no general
physical-memory direct-map — only the low 8MiB boot.asm identity-maps
is directly writable for new page-table bootstrap frames (checked at
runtime, panics if violated); heap is a fixed 1MiB with no growth-on-
demand yet. Revisit either only when something actually needs more
(a bigger heap, or mapping arbitrary physical memory like a device's
MMIO region).

## 5. PIT/APIC timer + IRQ handling — DONE
**Proves:** the kernel can be interrupted asynchronously by hardware on
its own schedule, not just run synchronously on demand — the foundation
Milestone 6's preemptive scheduler needs.
**Deliverables:**
- `kernel/drivers/pic.c/.h`: 8259 PIC remap (IRQ0-15 → vectors 32-47,
  ports/ICW values verified against Linux's own i8259 source), EOI,
  per-line mask/unmask. Legacy PIC+PIT chosen over APIC — confirmed with
  the user (roadmap said "PIT/APIC," ambiguous by design) — since APIC
  needs either ACPI MADT parsing (CLAUDE.md non-goal territory) or a
  hardcoded IOAPIC address, for benefits (per-CPU timers/IRQ routing)
  that don't matter without SMP (also a non-goal here). See ADR 0005.
- `kernel/drivers/pit.c/.h`: PIT channel 0, mode 3, programmable
  frequency (divisor verified against Linux's `PIT_TICK_RATE`); owns its
  own IRQ0 handler and a lock-free `volatile` tick counter (justified:
  single writer, single simple-load reader, atomic on x86_64).
- `kernel/arch/x86_64/irq.asm`, `common_stub.inc`: one stub per IRQ
  vector, sharing `isr.asm`'s exact save/align/restore sequence via a
  new shared NASM macro rather than duplicating it or hardcoding a
  struct-offset dispatch trick.
- `kernel/arch/x86_64/irq_dispatch.c/.h`: per-line handler registration
  and dispatch, called from `irq_common_stub`.
- `kernel/arch/x86_64/idt.c`: extended to also install IRQ gates
  (32-47) alongside the existing exception gates (0-31).
- `kernel/arch/x86_64/exceptions.c`: `isr_handler` now resumes normally
  for `#BP` specifically (traps are resumable by design) instead of
  halting like every other exception — lets the Milestone 2 self-test
  coexist with this milestone's requirement that the kernel keep
  running after boot.
- `kernel/kernel.c`: remaps the PIC, starts the PIT at 100Hz, unmasks
  IRQ0, enables interrupts, waits for 100 real ticks, then idles
  forever — the first milestone that doesn't end in a deliberate panic.
**Verification:** `make run` boots and prints the `#BP` fault dump
*followed by* (not ending in) `[OK] pic/pit initialized, timer IRQ0
unmasked` then `[OK] timer self-test passed (0x64 ticks received via
IRQ0)`. `tests/qemu/test_timer_irq_selftest.sh` (new) checks both
markers and that the `#BP` dump precedes the pic/pit line in the actual
output (proving resume-on-#BP works, not just present markers).
Milestones 1-4's smoke tests all re-verified passing.
**Design record:** `docs/adr/0005-pic-pit-irq-handling.md`.
**Known limitation (accepted for this milestone only):** only IRQ0 has
a registered handler — no keyboard (IRQ1) or other device driver yet;
`irq_register_handler` supports exactly one handler per line, no
chaining (nothing needs to share a line yet).

## 6. Preemptive scheduler + context switch (single CPU) — DONE
**Proves:** the kernel can forcibly preempt a running task and resume a
different one, on the timer's schedule, not the task's own — the
foundation Milestone 7's process model needs.
**Deliverables:**
- `kernel/sched/task.c/.h`: `task_t` (rsp, next, id) and `task_create()`,
  which builds a synthetic `trap_frame_t` on a fresh 16KiB kmalloc'd
  stack so a never-yet-run task can be resumed through the exact same
  `iretq` path a real interrupt uses.
- `kernel/sched/scheduler.c/.h`: round-robin ready queue (circular
  linked list), owns IRQ0 (calls `pit_tick()` itself), `scheduler_init`/
  `scheduler_add_task`.
- `kernel/arch/x86_64/common_stub.inc`: changed so `isr_handler`/
  `irq_handler` return the `trap_frame_t*` to actually resume, instead
  of the stub always restoring the one it saved — this is what makes a
  context switch possible with no separate save/restore mechanism.
  `isr_handler` always returns the same frame it was given (exceptions
  never switch tasks); only the scheduler's IRQ0 handler can return a
  different one.
- `kernel/drivers/pit.c/.h`: no longer self-registers an IRQ0 handler;
  now a plain hardware driver (`pit_init`/`pit_tick`/`pit_get_ticks`)
  the scheduler depends on, not the other way around.
- `kernel/kernel.c`: two demo kernel threads that never voluntarily
  yield (`for(;;) counter++`, no `hlt`), proving *forced* preemption
  specifically, not just cooperative switching.
**Verification:** `make run` boots and prints (after all Milestones
1-5 markers, unchanged) `[OK] scheduler initialized, 2 demo tasks
created` then `[OK] scheduler self-test passed, task A: 0x8b77079, task
B: 0x8812a58 (both made progress under preemption)` — ~146M vs ~143M
increments, within 2.4% of each other, itself evidence of *fair*
round-robin, not just "both nonzero." `tests/qemu/
test_scheduler_selftest.sh` (new) independently verifies both counters
are nonzero from the real output. All five earlier milestones' smoke
tests re-verified passing, confirming the `common_stub.inc` signature
change didn't regress exception/IRQ handling.
**Design record:** `docs/adr/0006-preemptive-scheduler.md`.
**Known limitation (accepted for this milestone only):** kernel threads
only, single shared address space (no ring 3 — Milestone 7); no
priorities or blocking (every task is always "in the rotation"); fixed
16KiB stack per task, no guard page or overflow detection yet.

## 7. Userspace: ring 3, syscalls, process model
## 8. Later: FS, drivers, SMP (sequence TBD from what's learned above)

Milestones 7–8 are intentionally left as one-line placeholders here — full
breakdown (deliverables/acceptance criteria/estimates/risks) gets written
up when that milestone actually starts, not in advance, to avoid designing
against assumptions already-implemented milestones might overturn.
