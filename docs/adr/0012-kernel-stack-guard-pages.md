# ADR 0012: Kernel stack guard pages

## Status
Accepted and verified — `make run` boots the real ISO, every kernel-
mode stack (kernel threads via `task_create()`, a ring-3 process's
kernel-mode stack via `task_create_user()`) now lives in its own
dedicated, page-mapped VA region with a deliberately-unmapped guard
page immediately below it, confirmed live by reading back real page-
table state. Landed correctly on the first real boot, same as ADR 0010
and ADR 0011 — verified further with a `-d int,cpu_reset` trace showing
zero page faults across a full boot (only the deliberate `#BP` self-
test), and all interrupt/exception entries landing on RSP values inside
the new dedicated region as expected.

## Context
Fourth step of the post-Milestone-8 "build this into an OS" inventory
— "guard pages," the last of the memory-maturity items explicitly named
alongside NX (ADR 0011) in that original list. Every kernel-mode stack
in this kernel — a kernel thread's whole stack, and a ring-3 process's
separate kernel-mode stack (used for `TSS.RSP0`/syscall entry) — came
straight from `kmalloc()`, packed with zero gap between allocations the
same as any other heap object. A stack overflow (deep recursion, a
runaway interrupt storm, or simply a bug) would silently corrupt
whatever heap allocation happened to sit right below it instead of
producing an immediate, diagnosable fault — exactly the "know the stack
size for every context" risk CLAUDE.md's safety rules call out, just
not yet closed for the kernel-mode side (a ring-3 process's own
USER-mode stack already got this protection for free as a side effect
of ADR 0009/0010's per-process design — see Decision).

## Decision
- **Every kernel-mode stack now gets its own dedicated, page-mapped VA
  slot in a new region, `PML4[511]:PDPT[509]` (`0xFFFFFFFF40000000`),
  preceded by one deliberately-unmapped guard page.** Distinct from the
  kernel image (`PDPT[510]`, boot.asm) and the kernel heap (`PDPT[511]`,
  heap.c); verified via `python3` before writing the constant, the same
  discipline every other PML4/PDPT index in this codebase already
  follows. A downward overflow now hits that unmapped page and takes an
  immediate `#PF` instead of silently corrupting an adjacent heap
  object.
- **`kernel/sched/task.c`'s `alloc_kernel_stack()`/
  `task_free_kernel_stack()` are shared by BOTH `task_create()` (kernel
  threads) and `task_create_user()` (a process's kernel-mode stack)** —
  the same underlying risk applied equally to both, so one mechanism
  serves both call sites rather than duplicating the guard-page logic.
  Each mapped page also gets `VMM_FLAG_NX` (data, never code — the same
  W^X reasoning ADR 0011 already applied to the kernel heap and a
  process's user-mode stack).
- **VA slots are handed out by a simple monotonic bump allocator and
  are never reclaimed/reused even after a task is reaped.** The same
  "simple now, revisit only if actually exhausted" scope boundary
  `heap_init()`'s fixed-size 1MiB region already accepted (ADR 0004): a
  1GiB region divided into ~20KiB slots (4KiB guard + 16KiB stack) is
  nowhere near exhausted by anything this kernel creates.
- **`vmm_translate()` (new, `vmm.h`/`vmm.c`) is a general VA->PA lookup**
  — needed because `vmm_unmap_page()` deliberately doesn't free the
  physical frame it unmaps (by design, see its own doc comment: freeing
  is kept a separate, explicit caller responsibility). Reused for the
  guard-page self-test too (see Verification) rather than being purely
  teardown-only infrastructure.
- **A ring-3 process's own USER-mode stack effectively already had
  guard-page protection, discovered rather than deliberately designed:**
  nothing is ever mapped immediately below `USER_STACK_VIRT_BASE` in a
  process's sparse address space (ADR 0009's PML4-index-1 region only
  ever holds code and stack, nothing else), so a downward overflow there
  already faults on genuinely unmapped memory today. Documented
  explicitly in `task.c` now rather than left as an unstated
  coincidence, so a future change that starts placing something else
  adjacent to it doesn't accidentally remove this property without
  anyone noticing.
- **Verification checks the resulting PTE state directly rather than
  triggering a live stack overflow and watching it fault** — the exact
  same constraint ADR 0011 already hit and documented: there's no
  exception-recovery mechanism yet to safely resume past a deliberately
  -triggered fault (it resumes AT the faulting instruction, unlike
  `#BP`). Flagged here again rather than re-litigated, since it's the
  same underlying gap, not a new one.

## Rejected alternatives
- **Only guard-page ring-3 processes' kernel-mode stacks, leave kernel
  threads' stacks on `kmalloc()`** — rejected: the underlying risk
  (silent adjacent-heap corruption on overflow) is identical for both;
  kernel threads aren't inherently safer just because they're
  "trusted," and unifying both onto one mechanism was no more work than
  building it for one and not the other.
- **Reclaim/reuse VA slots when a task is reaped**, to keep the
  dedicated region's footprint bounded — deferred as unnecessary
  complexity for what this kernel actually creates today (a handful of
  tasks total); would be revisited if something started creating and
  destroying large numbers of processes in a way that actually
  approached exhausting the 1GiB region, which nothing here does.
- **A guard page below the kernel heap itself, or splitting the heap
  into per-allocation guarded regions** — out of scope: the heap's
  first-fit allocator (`libk/heap_alloc.c`) manages one contiguous
  mapped region by design, and giving every individual allocation its
  own guard page would mean abandoning that allocator's whole model,
  a much larger change than this milestone's "kernel-mode stacks specifically"
  scope.

## Verification
- `make run` (real toolchain) boots and prints, after `[OK] tss/syscall
  initialized`: `[OK] guard page self-test passed (kernel stack guard
  page is unmapped)` — confirmed via `vmm_translate()` returning false
  for the page immediately below a live kernel thread's
  `kernel_stack_base`, not eyeballed. Every Milestone 1-11 marker still
  prints unchanged afterward, through the shell prompt.
- `-d int,cpu_reset -D build/qemu_debug.log` trace across a full boot:
  zero `#PF`/`#DF` events (`grep "v=0e\|v=08"` -> only the one
  deliberate `#BP`, `v=03`), and every `IRQ0` timer-tick entry's `SP`
  value observed inside the new `0xFFFFFFFF40xxxxxx` region — direct
  confirmation interrupts are actually landing on the new dedicated
  stacks, not stale kmalloc'd ones.
- `tests/qemu/test_guard_page_selftest.sh` (new): asserts the self-test
  marker, that it ran before the scheduler starts preempting tasks
  (real ordering, not assumed), and that the shell still starts
  normally afterward.
- The process-lifecycle leak check (ADR 0010) still passes with the new
  scheme: a process's kernel-mode stack now consumes real `pmm`
  frames directly (previously hidden inside the heap's own fixed
  footprint), and `task_free_kernel_stack()`'s frees correctly balance
  that -- `pmm_frames_free()` still returns to exactly its pre-creation
  baseline after both processes exit and are reaped, a strictly
  stronger check than before (it now also covers kernel-stack frames,
  not just address-space/user-stack frames).
- All eleven earlier smoke tests (through ADR 0011's NX test) re-run
  and pass. All three host tests re-run and pass. Booted 4 times back
  to back with identical output — no flakiness.
