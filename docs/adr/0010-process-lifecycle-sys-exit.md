# ADR 0010: Process lifecycle -- sys_exit and teardown

## Status
Accepted and verified — `make run` boots the real ISO, both ring-3
demo processes run their bounded workload, call `sys_exit`, and get
fully torn down (address space, both stacks, the task struct) with
zero physical frames leaked, confirmed by an exact before/after
`pmm_frames_free()` comparison. Unlike ADR 0009, this landed correctly
on the first real boot — the design was worked out by reasoning through
ADR 0009's lesson (don't free/switch away from state you're still
executing on) before writing any code, not discovered by crashing.

## Context
Milestone 9 gave every ring-3 process its own address space but no way
to ever stop running one — `task_create_user()`'s processes looped
`sys_nop` forever, and nothing in the kernel could remove a task from
the ready queue or reclaim its resources. That's the next item on the
"build this into an OS" inventory: a process model needs processes
that can actually finish, not just start.

## Decision

- **`sys_exit` never returns to user code, and never returns from
  `syscall_dispatch` either.** It marks the calling task `TASK_ZOMBIE`
  (a new `task_state_t` on `task_t`), re-enables interrupts (SYSCALL
  entry masks IF; nothing has re-enabled it yet at this point), and
  halts in an infinite loop — `scheduler_exit_current()`, declared
  `__attribute__((noreturn))` so the compiler enforces the contract.
  This deliberately reuses the exact same preemption path a normal
  timer tick already uses correctly: the next IRQ0 fires into the
  already-correct `common_stub.inc`/`timer_tick_handler` machinery, on
  the zombie's own kernel stack, exactly like preempting any other
  task. No new resume/switch mechanism was needed.
- **The actual resource teardown (freeing the address space, both
  stacks, the task struct) happens on a DIFFERENT task's stack, in a
  dedicated reaper kernel thread — never inside `timer_tick_handler`
  itself, and never inside `sys_exit`.** At the exact point
  `timer_tick_handler` notices a task is a zombie, it's still running
  ON that zombie's own kernel stack and CR3 is still that zombie's
  PML4 (both only actually change later, in `common_stub.inc`, after
  the C handler returns) — freeing either there would be a use-after-
  free of state still in active use, the same category of hazard ADR
  0009's Bug 1 hit. `timer_tick_handler` only unlinks the zombie from
  the ready queue and hands it to a separate chain; a reaper task
  (spawned by `scheduler_init()` itself, since exit doesn't work
  without it) drains that chain on its own stack, where the zombie's
  resources are guaranteed no longer in use.
- **The ready queue became doubly-linked (`task_t` gained `prev`)** so
  a zombie can be unlinked in O(1) without a predecessor search. The
  zombie hand-off chain reuses the same `next` field (a task is never
  in both lists at once, so this isn't a real conflict) rather than
  adding a fourth pointer field, on the same "small, explicit, not more
  than needed" reasoning as everything else in `task_t`.
- **A new per-leaf-mapping PTE flag, `VMM_FLAG_OWNED` (bit 9, one of
  the hardware-defined AVL/available-for-OS-use bits), distinguishes a
  process's own pmm-allocated memory (its stack) from memory it merely
  maps but doesn't own (the demo program's code page, which lives
  in the kernel image itself and was never `pmm_alloc_frame()`'d).**
  `vmm_destroy_address_space()` only `pmm_free_frame()`s a leaf's
  target when this bit is set — needed because the SAME physical code
  page is deliberately shared read-only across every process (ADR
  0009); freeing it when one process exits would silently corrupt or
  double-free memory every other still-running process (and the next
  process created after this frame is potentially reused for something
  else) still depends on.
- **Teardown walks every PML4 entry except `[0]` and `[511]`** (the
  identity map and kernel half shared with the kernel and every other
  address space, ADR 0009) and recursively frees every intermediate
  PDPT/PD/PT frame unconditionally (always pmm-owned, since
  `get_or_create_table` always allocates them fresh per address space)
  plus every `VMM_FLAG_OWNED` leaf, then the PML4 frame itself.
  Panics defensively if it ever finds a huge (2MiB/1GiB) page in a
  process-private region — nothing in this codebase creates one there,
  so hitting one would mean a logic bug elsewhere, not something to
  silently mishandle.
- **The demo program (`user_demo.asm`) now runs a bounded `sys_nop`
  loop (200,000 iterations) instead of an infinite one**, then calls
  `sys_exit` — otherwise there'd be nothing for this milestone's
  self-test to actually observe. The loop counter has to live in RBX,
  not RCX: the `SYSCALL` instruction itself clobbers RCX (return RIP)
  and R11 (return RFLAGS), the one ABI detail that survives a syscall
  differently from every other GPR (verified against the existing
  `syscall_frame_t` doc comment, not re-derived from scratch).
- **`kernel_main`'s self-test captures `pmm_frames_free()` before
  either process is created**, waits (the same
  hlt-until-a-counter-advances pattern every earlier milestone's
  self-tests use) for `scheduler_reaped_count() == 2`, and panics if
  the frame count hasn't returned to exactly that baseline. This is a
  precise, objective leak check, not just "didn't crash while
  freeing" — matching the standard every earlier milestone's self-test
  already holds itself to (e.g. Milestone 3's alloc/free/reuse check).

## Rejected alternatives
- **Free a zombie's resources synchronously inside `sys_exit` or
  `timer_tick_handler`** — rejected outright: both run on the exiting
  task's own stack/CR3 at the point they'd need to do this, so it's
  exactly ADR 0009's Bug 1 hazard again. A reaper task, with its own
  independent stack, sidesteps the problem entirely by construction
  rather than requiring careful sequencing within a single interrupt.
- **Give every process its own private copy of the demo code page**
  instead of adding the ownership flag — would work, but doubles the
  cost of every process for no reason and throws away ADR 0009's
  code-sharing design; the ownership-bit approach keeps sharing while
  still tracking accountability correctly, and generalizes cleanly to
  a future real ELF loader (a process's actually-private code pages
  would just get `VMM_FLAG_OWNED` too).
- **A parent/`wait()`/exit-code-reporting mechanism** — out of scope
  for this step; nothing in the kernel has a parent-process concept
  yet (every process here is spawned directly by `kernel_main`, not by
  another process), so there's no one to report an exit code to. Left
  for whenever `fork`/process creation from userspace exists.

## Verification
- `make run` (real toolchain) boots and prints all Milestones 1-9
  markers unchanged, then both processes' `sys_write` messages, then
  (once each finishes its 200,000-iteration loop) `[OK] process 4
  exited and was reaped` / `[OK] process 5 exited and was reaped`, then
  `[OK] process lifecycle self-test passed, ... frames free, matches
  pre-creation baseline` with the exact same free-frame count printed
  both before creation and after full teardown -- confirmed by direct
  before/after `pmm_frames_free()` comparison in the code, not eyeballed.
  Booted 4 times back to back with identical output each time -- no
  flakiness, and (unlike ADR 0009) no crash on the very first attempt.
- `tests/qemu/test_process_lifecycle_selftest.sh` (new): asserts
  exactly two "exited and was reaped" lines, the self-test's own
  pass line, that the two processes' PML4s were confirmed distinct
  beforehand (guards against silently regressing to a shared-address-
  space design and still passing), and that the shell still starts
  normally afterward.
- All nine earlier smoke tests (including Milestone 9's
  `test_process_isolation_selftest.sh`, which still passes unmodified
  -- the "hello from ring 3" count and PML4-difference checks are
  unaffected by processes now also exiting afterward) re-run and pass.
  All three host tests re-run and pass.
