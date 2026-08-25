# ADR 0025: General blocking/wake scheduler primitive

## Status
Accepted and verified — `make run` boots the real ISO and prints both
new self-test markers in the correct order, with a real, deterministic
(not timing-tuned) proof that a task genuinely left the ready queue
while blocked and only resumed after `scheduler_wake()` was explicitly
called on it. All twenty-three pre-existing smoke tests plus the new
`test_scheduler_wake_selftest.sh` pass; all four host test suites pass.
`-d int,cpu_reset` trace unchanged from Milestone 24 (1 `#BP`, 3 `#PF`,
zero double-fault/reset). **A real bug was found and fixed during
verification** — the first genuine live-boot bug since Milestone 16 —
see Verification below for the full diagnostic trail. Booted 5 times
back to back with identical shape every time after the fix.

## Context
`Desktop.md` sequences this as the second step of the GUI arc, right
after the userspace C runtime (Milestone 24). `future.md` had already
flagged the need: `sys_wait`'s own blocking design (Milestone 20, ADR
0020) is a one-off `sti;hlt;cli` retry loop scoped to just that one
syscall — the caller stays `TASK_READY` the whole time, consuming a
wasted scheduler turn on every round just to re-check a condition that
hasn't changed. That was the right call for a single caller with no
general primitive to build on; it stops being the right call once a
GUI event loop and IPC (Milestone 26) need the same "block until
something else wakes me" shape, potentially with many tasks blocked
simultaneously (a window server waiting for client messages while N
client apps are each waiting for something themselves).

## Decision

- **`TASK_BLOCKED`, a new `task_state_t` value**, alongside the
  existing `TASK_READY`/`TASK_ZOMBIE`. A blocked task is unlinked from
  the ready queue by the SAME mechanism, at the SAME point, as a
  zombie task already is — `timer_tick_handler`'s existing
  `outgoing->state == TASK_ZOMBIE` check gained an `else if
  (outgoing->state == TASK_BLOCKED)` branch doing the identical
  prev/next unlink, just without moving the task onto `zombie_head`
  (see next point for why it doesn't need an equivalent list).
- **No blocked-task list of its own, unlike `zombie_head`.** The
  reaper is a generic CONSUMER that doesn't know in advance which
  zombies will appear, so it needs a producer/consumer queue.
  `scheduler_wake(task_t *task)` is always called by a caller that
  already holds the specific `task_t*` to wake (e.g. whatever resource
  the task was waiting on stored it directly) — there is no "search for
  something to wake" case this milestone needs to support, so no
  searchable list was built for one.
- **`scheduler_block_current()`: marks the caller `TASK_BLOCKED`, `sti`,
  loops `hlt` until woken, `cli` before returning.** Once unlinked (on
  the next tick), the task consumes ZERO further scheduler turns until
  `scheduler_wake()` relinks it — a real efficiency change from
  `sys_wait`'s old polling design, not just a rename. Contract:
  callable only from a task's own normal execution context (matching
  `sys_wait`'s existing precedent, mid-syscall with `IF=0` already),
  and ALWAYS returns with interrupts disabled again, mirroring the
  exact in/out invariant `sys_wait`'s own retry loop already had.
- **`scheduler_wake(task_t *task)`: a no-op unless `task->state ==
  TASK_BLOCKED`, otherwise relinks it via `scheduler_add_task()`
  (reused verbatim — the insertion logic is identical).** Designed to
  be safe from either a normal task context OR a future
  interrupt-handler context (no current caller needs the latter yet,
  but an IRQ-driven wake — e.g. keyboard input unblocking a waiting
  reader — is a foreseeable next consumer, and getting this right now
  costs one extra instruction, not a new design). This is exactly what
  went wrong on the first implementation attempt — see Verification.
- **Not rewired into `sys_wait` this milestone.** `sys_wait`'s existing
  polling design still works correctly (ADR 0020) and rewiring it onto
  the new primitive is optional future polish, not something THIS
  milestone's actual goal (proving the general primitive exists and
  works, ready for Milestone 26's IPC to use it for a blocking receive)
  needs. Keeps "one subsystem per change" clean — building the
  primitive and proving it in isolation, without simultaneously risking
  a regression to the already-solid fork/wait test suite.
- **A dedicated two-kernel-thread self-test with an explicit go/no-go
  handoff flag, not a tuned delay.** `block_test_blocker` sets
  `block_test_reached_block_point` immediately before blocking;
  `kernel_main` observes that flag, confirms `block_test_woke_up` is
  STILL false (proving a genuine block, not an instant no-op), THEN
  sets `block_test_observed_still_blocked` to explicitly release
  `block_test_waker` to actually call `scheduler_wake()`. Ordering is
  deterministic BY CONSTRUCTION — a strictly more robust technique than
  Milestone 20's own tuned-delay approach (ADR 0020), made possible
  here because this test controls BOTH sides of the race (its own two
  dedicated threads) rather than reusing an unrelated demo program's
  independent timing.

## Rejected alternatives
- **A searchable global blocked-task list**, mirroring `zombie_head`.
  Rejected — see Decision; no current or foreseen caller needs to
  "search for something blocked," only "wake this specific task I
  already have a pointer to."
- **Rewiring `sys_wait` onto the new primitive in this same milestone.**
  Rejected as unnecessary scope for what this milestone actually needs
  to prove — see Decision. Left as an explicit Known limitation, not
  silently dropped.
- **A tuned real-tick delay for `block_test_waker`** (the same
  technique ADR 0020 used), initially considered before writing any
  test code. Rejected in favor of the explicit handoff flag once it
  became clear this test — unlike Milestone 20's, which had to
  synchronize against an INDEPENDENT demo program's own unrelated
  timing — controls both sides of the interaction directly, so a
  fully deterministic design was available and strictly better.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-24
  marker unchanged, plus (new) `[OK] blocking/wake self-test: task
  genuinely blocked, confirmed not yet woken` strictly before `[OK]
  blocking/wake self-test passed, task correctly resumed after
  scheduler_wake()`. `kernel_main` PANICs if the blocker is ever
  observed already woken at the "still blocked" checkpoint.
- **A real bug was found and fixed during verification, diagnosed by
  reasoning through the actual observed symptom, not guessed.** The
  first real boot attempt hung completely after the "still blocked"
  message — not just the block/wake test stalling, but EVERY other
  unrelated task's progress (fork/wait, COW, process reaping) also
  stopping dead at exactly that point, which is the tell: a single-CPU
  machine-wide freeze, not a logic bug isolated to one test. Root
  cause: `scheduler_block_current()`'s documented contract ("always
  returns with `IF=0`") was correct for `sys_wait`'s syscall-context
  needs (a syscall's own `sysret` naturally restores the right flags
  afterward) but `block_test_blocker` is a KERNEL THREAD, not
  mid-syscall — after resuming, it fell straight into its own trailing
  `hlt` loop with interrupts still disabled, which halts a single-CPU
  machine forever (`hlt` with `IF=0` waits for an NMI that never
  comes, so no timer tick can ever fire again for ANY task). Fixed by
  adding an explicit `sti` in `block_test_blocker` before its own
  trailing loop — the exact same pattern `scheduler_exit_current()`
  already uses before ITS trailing `hlt` loop, for the identical
  reason; this precedent existed in the codebase already and should
  have been applied the first time, not discovered by hanging the
  machine. **A second, separate bug was caught before it ever reached a
  boot attempt**, during code review of the first implementation draft
  of `scheduler_wake()`: a raw `pushfq` held open across intervening C
  code (`if (task->state == ...) { scheduler_add_task(task); }`) before
  a later `popfq` — GCC has no idea a bare `pushfq` shifted `RSP` by 8,
  so any stack-relative local/spill it emits in that window (legal even
  at `-O0`) would silently read or write the wrong slot until `popfq`
  restored `RSP`. Fixed by capturing the flags into an ordinary C
  variable via `pushfq; pop %0` inside a SINGLE atomic asm block
  (net `RSP` effect zero, nothing held open across a C-code boundary)
  before touching any C code, restoring `IF` conditionally at the end
  based on that saved value instead of a second raw `popfq`.
- `-d int,cpu_reset` trace across a full boot (after both fixes):
  unchanged from Milestone 24 (1 `#BP`, 3 `#PF`, zero double-fault/
  reset) — this milestone's new code paths never fault.
- `tests/qemu/test_scheduler_wake_selftest.sh` (new): checks both
  markers, absence of the panic's own text, and correct ordering
  (the "still blocked" line strictly before the "resumed" line) — plus
  re-checks the process-lifecycle frame-leak self-test still passes
  (the test threads never exit and hold no `pmm`-tracked resources
  beyond their own kernel stack, same as the Milestone 6 demo tasks,
  so this is an independent confirmation of no regression, not a new
  risk). All twenty-three earlier smoke tests and all four host test
  suites re-verified passing. Booted 5 times back to back after the
  fixes — identical shape every time.

## Known limitations (accepted for this milestone only)
`sys_wait` still uses its own Milestone 20 polling loop, not this
primitive — a deliberate scope boundary (see Decision), not an
oversight; rewiring it is optional future work with no concrete benefit
this milestone needs. No priority/fairness policy for waking multiple
blocked tasks waiting on the same resource (not a real gap yet — no
current caller has more than one task blocked on the same thing at
once; Milestone 26's IPC is the first candidate that might). No timeout
support (`scheduler_block_current()` blocks until explicitly woken,
with no "give up after N ticks" option) — not needed by any current
caller; would be straightforward to add on top of the existing
`TASK_BLOCKED` state if a future milestone needs it.
