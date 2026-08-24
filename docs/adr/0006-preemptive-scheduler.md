# ADR 0006: Preemptive round-robin scheduler via trap-frame stack switching

## Status
Accepted and verified (Milestone 6) — `make run` boots the real ISO and
two never-yielding demo tasks genuinely share the CPU under forced
preemption; see Verification.

## Context
Every task in this kernel so far has been `kernel_main` itself, running
synchronously from boot to idle loop. A scheduler needs a fundamentally
different capability: saving one execution context and resuming a
different one, triggered involuntarily by the timer (Milestone 5) —
"preemptive," as opposed to a task voluntarily giving up the CPU.

## Decision

- **Reuse the interrupt trap frame itself as the context-save format —
  no separate context-switch save/restore routine.** Every task's
  complete register state is already a `trap_frame_t` sitting on that
  task's own stack: either a real one left by the timer preempting it
  mid-execution, or a synthetic one `task_create()` builds up front to
  *look* like one. A context switch is then nothing more than the
  timer's handler returning a different `trap_frame_t*` than the one it
  received — `common_stub.inc`'s existing `iretq` path resumes whichever
  frame comes back, uniformly, whether that's the same task or a
  different one. This avoids building and maintaining a second,
  parallel save/restore mechanism (e.g., a callee-saved-only
  `switch_context(old_rsp, new_rsp)` routine) alongside the one that
  already exists for interrupts.
- **`common_stub.inc` changed: `isr_handler`/`irq_handler` now return
  `trap_frame_t*`, loaded into `RSP` before the final `iretq`, instead
  of the macro always restoring the frame it saved.** This is the one
  piece of Milestone 2/5 machinery this milestone had to modify, not
  just build on top of. `isr_handler` (exceptions.c) always returns the
  same frame it was given — exceptions never trigger a task switch —
  so Milestones 2/5's behavior is provably unchanged (re-verified: all
  five earlier smoke tests still pass). Verified the macro change itself
  by disassembly before writing anything on top of it.
- **Kernel threads only — single shared address space, no ring 3.**
  Matches roadmap sequencing (userspace/ring 3 is Milestone 7, not this
  one). Every task's synthetic frame uses the same `KERNEL_CODE_SELECTOR`/
  `KERNEL_DATA_SELECTOR` as the rest of the kernel and shares `CR3` —
  there's no address-space switch anywhere in this design.
- **Round-robin, no priorities, no blocking.** A circular linked list of
  `task_t`; the timer handler saves the current task's frame pointer,
  advances to `->next`, and returns that task's saved frame. Nothing yet
  needs anything more sophisticated — no priority levels, no blocking
  primitives (a task can't wait on anything yet), so building either
  would be speculative. `task_t` itself stays minimal (`rsp`, `next`,
  `id`) for the same reason — no `state` enum, since every task is
  always simply "in the rotation."
- **Fixed 16KiB stack per task, allocated via `kmalloc`.** CLAUDE.md:
  know the stack size for every context. These are small, non-recursive
  demo/self-test tasks; 16KiB is generous headroom, not a tuned number.
  `TASK_STACK_SIZE` is a `#define`, easy to revisit once a real
  workload's stack needs are known.
- **`task_create()` builds a synthetic `trap_frame_t` via a designated
  initializer** (`(trap_frame_t){.rip=entry, .cs=..., .rflags=0x202,
  .rsp=stack_top, .ss=...}`), leaving every unlisted field (all GPRs,
  vector, error_code) zeroed by C99's designated-initializer rule
  rather than assigning each individually — correct because a
  never-yet-run task has no real prior register state to restore, and
  less error-prone than 15 explicit `= 0` lines.
- **`rflags = 0x202`**: bit 9 (IF) set, so a task starts with interrupts
  enabled (matching every other kernel context), bit 1 (an
  always-reserved-1 bit, not a real flag) set because the architecture
  requires it. No other flag bits are meaningful at task-creation time.
- **`kernel/sched/scheduler.c` owns IRQ0, not `kernel/drivers/pit.c`.**
  `pit.c` lost its self-registered handler; it now just programs the
  hardware and exposes `pit_tick()`/`pit_get_ticks()`. The scheduler's
  own IRQ0 handler calls `pit_tick()` itself before deciding whether to
  switch tasks. This keeps `pit.c` a plain hardware driver with no
  scheduling-policy dependency — the dependency runs the correct
  direction (scheduler depends on the timer driver, not the reverse).
- **The bootstrap task's `rsp` starts at 0 and is filled in lazily**, the
  first time `kernel_main`'s own context is actually preempted (which
  happens naturally, the same way it would for any task — the timer
  handler unconditionally does `current_task->rsp = frame` before
  advancing). No special-casing needed for "the task that was already
  running when the scheduler started."
- **Demo tasks (`kernel_main`'s `demo_task_a`/`demo_task_b`) never
  voluntarily yield** (`for (;;) { counter++; }`, no `hlt`, no blocking
  call) — deliberately, to prove *forced* preemption. A cooperative or
  yield-based design could pass a weaker self-test even with a broken
  timer-driven path, if both tasks happened to yield on their own.

## Rejected alternatives
- **A separate `context_switch.asm` saving only callee-saved registers**
  (the classic `switch_context(old_rsp, new_rsp)` approach for
  voluntary/cooperative switches) — valid and simpler per-switch (fewer
  registers to save), but this milestone is specifically about
  *preemptive* switching, which already happens inside an interrupt
  context with the full frame saved regardless. Building a second
  mechanism on top would duplicate work the interrupt path already
  does. Worth reconsidering if a future voluntary `yield()` is added and
  the full trap-frame save proves wasteful for that path specifically.
- **Priority levels or multiple ready queues** — no basis yet for what
  priorities would even mean here; every task is equally "important"
  (there's exactly one real workload: prove preemption works).
- **Blocking/sleep primitives** — nothing exists yet for a task to
  block on (no I/O completion, no locks, no IPC). Adding a `TASK_BLOCKED`
  state now would be unused, speculative machinery.
- **A generic `timer.c` calling into the scheduler** — already rejected
  in ADR 0005 (no second timer backend exists to abstract over); this
  milestone's direct `scheduler.c` → `pit.c` dependency is the same
  reasoning applied one layer up.

## Verification
- `common_stub.inc`'s return-value-driven resume was verified by
  disassembly (`objdump -d`) before any scheduler code was written:
  confirmed `mov rdi, rsp` / `and rsp, ~0xf` / `call` / `mov rsp, rax`
  encodes as expected.
- `make run` (real toolchain) boots and prints, in order: all
  Milestones 1-5 markers unchanged, then `[OK] scheduler initialized, 2
  demo tasks created`, then (after the existing timer self-test) `[OK]
  scheduler self-test passed, task A: 0x8b77079, task B: 0x8812a58 (both
  made progress under preemption)` — ~146M and ~143M increments
  respectively, within 2.4% of each other. That closeness is itself
  meaningful evidence of *fair* round-robin behavior, not just "both
  nonzero by luck."
- `tests/qemu/test_scheduler_selftest.sh` (new): checks the init/self-
  test markers and independently extracts and verifies both task
  counters are nonzero from the actual captured output.
- `tests/qemu/test_boot_serial.sh`, `test_idt_selftest.sh`,
  `test_pmm_selftest.sh`, `test_heap_selftest.sh`, and
  `test_timer_irq_selftest.sh` (Milestones 1-5) all re-run and still
  pass — confirms the `common_stub.inc`/`isr_handler`/`irq_handler`
  signature changes didn't regress exception handling, IRQ dispatch, or
  the existing timer self-test.
