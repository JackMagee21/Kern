# ADR 0005: Legacy PIC + PIT, and hardware IRQ handling

## Status
Accepted and verified (Milestone 5) — `make run` boots the real ISO,
the timer self-test receives real ticks over IRQ0, and the `#BP`
self-test now resumes normally afterward; see Verification.

## Context
Every kernel service so far has run entirely synchronously inside
`kernel_main`, on demand. A timer requires something fundamentally
different: the CPU has to be interrupted *asynchronously* by hardware,
on its own schedule, which needs an interrupt controller routing
external IRQ lines to IDT vectors, and the IDT extended to actually
handle vectors beyond the 32 CPU exceptions Milestone 2 covered.

## Decision

- **Legacy 8259 PIC + 8253/8254 PIT, not the APIC.** Confirmed with the
  user directly (roadmap says "PIT/APIC," ambiguous by design). The PIC
  and PIT live at fixed, architecturally-guaranteed I/O ports — no
  discovery needed. A proper APIC setup means either parsing ACPI's
  MADT table to find the IOAPIC (CLAUDE.md lists ACPI power management
  as a non-goal — adjacent territory this avoids touching entirely) or
  hardcoding QEMU's conventional IOAPIC address, which isn't portable
  practice. The Local APIC's main advantages (per-CPU timers, multi-CPU
  IRQ routing) are also largely moot here since SMP is itself a
  CLAUDE.md non-goal. Revisit only if a real future need (SMP, or IRQ
  routing the PIC genuinely can't express) forces the issue.
- **Ports/constants verified against Linux's own kernel source**, not
  recalled from memory: `arch/x86/include/asm/i8259.h`
  (`PIC_MASTER_CMD=0x20`, `PIC_MASTER_IMR=0x21`, `PIC_SLAVE_CMD=0xa0`,
  `PIC_SLAVE_IMR=0xa1`), `arch/x86/kernel/i8259.c` (ICW1=0x11, ICW4
  8086-mode bit), `include/linux/timex.h`
  (`PIT_TICK_RATE=1193182`). Manual (not auto) EOI is used, unlike
  Linux's default — simpler/more explicit for a kernel not yet
  optimizing interrupt latency.
- **PIC remap masks every line by default; callers explicitly unmask
  what they handle.** Same "default deny" instinct as `pmm.c`'s memory
  map parsing (ADR 0003) — don't inherit whatever mask state firmware/
  GRUB happened to leave. Only IRQ0 (timer) is unmasked this milestone.
- **IRQ stubs (`irq.asm`) share `isr.asm`'s exact save/align/restore
  sequence via a new shared NASM macro (`common_stub.inc`)**, rather
  than duplicating it or building a runtime dispatch that reads the
  vector field out of the trap frame via a hardcoded byte offset to
  decide which C handler to call. The macro is DRY at the source level
  while each generated stub stays simple, self-contained, and free of
  a fragile magic offset into a struct that could silently drift if
  `trap_frame_t`'s layout ever changes. Verified the refactor changed
  nothing: disassembled `isr.asm`'s object before and after and diffed
  byte-for-byte identical (aside from the filename in the output).
- **`trap_frame_t.vector` stays the literal IDT vector (32-47 for
  IRQs), uniform across both `isr.asm` and `irq.asm`** — `irq_handler`
  subtracts `IDT_IRQ_VECTOR_BASE` itself to get the 0-15 line number,
  rather than having IRQ stubs push a different kind of "vector" value
  than exception stubs do.
- **`kernel/arch/x86_64/irq_dispatch.c`, not `irq.c`.** `irq.c` would
  compile to the same object path as `irq.asm` under this Makefile's
  pattern rules (`kernel/arch/x86_64/irq.{c,asm}` → both
  `build/kernel/arch/x86_64/irq.o`) — hit this for real during the
  build (silent object clobber, `ld` reported "multiple definition" and
  "undefined reference to irq0..irq15" simultaneously, which is exactly
  what a collision like that looks like). Named to match the existing
  `isr.asm`/`exceptions.c` precedent (mechanism vs. dispatch/policy, two
  different basenames) instead of introducing a new naming pattern.
- **`pit.c` owns its own IRQ0 handler and tick counter internally**
  (`pit_init` calls `irq_register_handler(0, pit_irq_handler)` itself),
  rather than a separate generic "timer" module — timer semantics are
  entirely PIT-specific at this point (frequency/divisor math, mode 3
  programming), so there's nothing generic to factor out yet.
- **`tick_count` is a plain `volatile uint64_t`, no lock.** Single
  writer (the IRQ0 handler, which can't be reentered while running —
  interrupt gates clear IF), single reader doing a simple load (not a
  read-modify-write) from normal context, and aligned `uint64_t`
  reads/writes are atomic on x86_64. This is CLAUDE.md's "justified
  lock-free structure" case for data shared with an interrupt handler,
  not a race — documented as such in `pit.h` rather than left implicit.
- **`isr_handler` now resumes normally (returns, letting `iretq`
  continue execution) for `#BP` specifically, instead of halting like
  every other exception.** `int3` is architecturally a trap, not a
  fault — the saved `RIP` already points past the `0xCC` byte, so
  resuming is both correct and the entire point of `int3` as a
  debugging primitive. This is what lets the Milestone 2 self-test
  coexist with Milestone 5's actual requirement (the kernel has to keep
  running after boot to service ticks) without reordering or dropping
  either self-test. Every other exception is still unconditionally
  fatal — no general fault-recovery mechanism exists yet, this is
  specifically about `#BP`'s well-defined, resumable semantics.
- **No more deliberate terminal panic at the end of `kernel_main`.**
  Milestones 2-4 all ended by intentionally crashing (there was nothing
  else useful left to do). Milestone 5 is the first one where the
  kernel has a legitimate, ongoing reason to keep running — idling,
  servicing timer interrupts — so the boot sequence's steady state
  changes from "prove it, then panic" to "prove it, then idle forever."

## Rejected alternatives
- **APIC (LAPIC timer + IOAPIC)** — see Decision; confirmed against the
  user rather than assumed.
- **Auto-EOI mode** (Linux's default) — marginally less code per IRQ,
  but manual EOI is more explicit about exactly when the PIC is told an
  IRQ is finished, which matters more for a first IRQ-handling milestone
  than shaving a few instructions.
- **A generic `timer.c` abstraction over PIT** — no second timer
  backend exists to abstract over yet (APIC timer was explicitly
  rejected above); premature generalization for a single, concrete
  implementation.
- **Making `isr_handler` generically resumable/recoverable** for more
  than just `#BP` — every other exception is a genuine fault with no
  defined safe continuation without a scheduler/process model
  (Milestone 6+). Only `#BP`'s specific, well-known trap semantics
  justify resuming; this isn't a step toward general fault recovery.

## Verification
- `nasm -f elf64` assembles `irq.asm` cleanly; `irq0`/`irq15` spot-checked
  by disassembly (dummy-error-code-then-vector `0x20`/`0x2f`, matching
  the remapped vector range exactly).
- `common_stub.inc` refactor verified via `objdump -d` diff: disassembly
  of `isr.asm`'s object is byte-for-byte identical before and after,
  confirming the macro extraction changed nothing functionally.
- `make run` (real toolchain) boots and prints, in order: the `#BP`
  fault dump (still correct: vector `0x3`, `cs=0x8`/`ss=0x10`) — followed
  by, not ending in, `[OK] pic/pit initialized, timer IRQ0 unmasked`
  then `[OK] timer self-test passed (0x64 ticks received via IRQ0)`
  (`0x64` = 100, exactly `TIMER_SELFTEST_TARGET_TICKS`) — proving
  `isr_handler`'s new resume-on-#BP behavior actually works, not just
  that it compiles.
- `tests/qemu/test_timer_irq_selftest.sh` (new): checks both self-test
  markers AND that the `#BP` dump's line number precedes the pic/pit-
  initialized line in the captured serial output — a direct assertion
  that execution actually continued past the breakpoint, not an
  incidental byproduct of marker presence.
- `tests/qemu/test_boot_serial.sh`, `test_idt_selftest.sh`,
  `test_pmm_selftest.sh`, and `test_heap_selftest.sh` (Milestones 1-4)
  all re-run and still pass — confirms the `isr_handler` behavior change
  and the new IDT/IRQ gates didn't regress anything earlier.
