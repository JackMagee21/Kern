# ADR 0020: Genuinely blocking sys_wait()

## Status
Accepted and verified — `make run` boots the real ISO, the new
`syscall_get_wait_block_count()` self-test confirms sys_wait actually
took the blocking path (not just that it eventually returned the right
answer), and all nineteen pre-existing smoke tests plus all four host
test suites re-verified passing with no regression. Verified via
`-d int,cpu_reset` (zero unexpected `#PF`/double-fault/reset events,
only the pre-existing deliberate `#BP` self-test and expected IRQ
traffic). Correct on the first real boot attempt; booted 5 times back
to back with identical shape (modulo the exact block-turn count, which
varies run to run by design — see Verification).

## Context
ADR 0018 shipped `sys_wait` deliberately non-blocking, with a named
reason: this kernel's syscalls run fully non-preemptible, interrupts
masked for the entire duration (`SFMASK`, ADR 0007) — a genuinely
blocking wait needs to sleep with interrupts enabled and a way for
something else to eventually resume it, neither of which existed. The
caller (`kernel/user/fork_demo.asm`) polled instead: repeated
`sys_wait` calls with a short `sys_nop` spin in between. `future.md`
flagged this as the natural next "synchronization/IPC" item, now with
a concrete motivating case rather than IPC in the abstract.

## Decision

- **No new scheduler primitive (no `TASK_BLOCKED` state, no wake-list).**
  `sys_wait`'s own loop just does `sti; hlt; cli` and re-polls
  `scheduler_try_wait()` once per iteration, relying entirely on the
  ALREADY-EXISTING preemptive round-robin scheduler (Milestone 6) to
  give every other task (including the reaper, which is what actually
  produces a match) its turns in between. The calling task stays
  `TASK_READY` in the ordinary ready queue the whole time — from the
  scheduler's point of view it looks exactly like any other task that
  happens to be doing nothing useful on most of its turns, no different
  in kind from `reaper_task`'s own pre-existing `hlt`-when-idle loop.
  Rejected building a real primitive (explicit blocked state, a
  `wake(task)` call, per-event wait-queues) because `sys_wait` is
  currently the ONLY syscall that would ever use it — that generality
  is exactly the right shape for a FUTURE real IPC/synchronization
  milestone once there's a second real caller motivating it (see
  `future.md`), not this one.
- **Found and fixed a genuine latent bug this change would otherwise
  have exposed:** `syscall_entry.asm`'s `saved_user_rsp` was a single
  bare global, written unconditionally by EVERY syscall entry and read
  back at every syscall exit. Safe ONLY because syscalls were
  previously atomic w.r.t. scheduling — no other task's own syscall
  could ever interleave. The moment `sys_wait` can genuinely block with
  interrupts enabled, a SECOND, completely unrelated task's own
  syscall (entered and exited normally while the first sits blocked)
  would clobber the shared global with its own value before the first
  task ever reads its own back — corrupting the first task's `sysretq`
  target (jumping to the WRONG user-mode stack on resume). Fixed by
  moving the durable copy into `task_t` itself (`saved_user_rsp`, new
  field, `kernel/sched/task.h`) plus a scheduler-maintained indirection
  pointer (`syscall_user_rsp_slot`, `kernel/arch/x86_64/syscall.c`,
  repointed at the CURRENT task's own field on every context switch in
  `scheduler.c`'s `timer_tick_handler` — the exact same per-task
  redirection pattern this codebase already uses for
  `syscall_kernel_rsp`/`TSS.RSP0`). `syscall_entry.asm`'s OWN bare
  `saved_user_rsp` symbol still exists, but is now only ever used as
  TRANSIENT scratch for the brief register-free handoff between
  "value fetched via the per-task indirection" and "value consumed by
  `mov rsp, ...`" — both windows run with `IF=0` (interrupts still
  masked: at entry nothing has `sti`'d yet; at exit `sys_wait`'s loop
  always `cli`'s again before returning), so no other task's syscall
  can interleave and clobber it mid-handoff. This is a correctness fix
  the OLD non-blocking design never needed and never exercised — found
  by reasoning through what "another task's syscall can now genuinely
  interleave" actually implies, not by hitting it as a live bug in
  QEMU.
- **Why preempting a syscall handler needed zero new cases in
  `common_stub.inc`.** A syscall's C-level work
  (`syscall_dispatch`/`sys_wait`) runs entirely on the calling task's
  OWN kernel stack (`task->kernel_stack_top`, already switched
  correctly per-task via `TSS.RSP0`/`syscall_kernel_rsp` since
  Milestone 7), and `SYSCALL`/`SYSRET` never switches `CR3` at all
  (ADR 0007) — so a timer interrupt landing inside `sys_wait`'s `hlt`
  is not a privilege or address-space transition of any kind. `CPL=0`
  either way, and the interrupt frame layout is unconditional (long
  mode always pushes `SS:RSP`, even for a same-privilege interrupt —
  Intel SDM Vol. 3A Sec. 6.14.2, the same fact this codebase already
  relies on to preempt ordinary kernel threads). Preempting a task
  that happens to be inside a syscall handler is therefore
  architecturally identical to preempting any kernel thread — already
  proven correct across Milestones 6-19 — not a new case.
- **A caller with no children (never forked) blocks forever.** Accepted
  known limitation: the same underlying gap ADR 0018 already flagged
  (no live per-parent child list, only after-the-fact
  zombie/collected tracking) — this milestone just changes HOW that
  gap manifests, from an infinite userspace poll loop to an infinite
  kernel-side block. Not a new regression.
- **`kernel/user/fork_demo.asm` simplified to a single `sys_wait` call**
  (its old poll-and-spin wrapper deleted) — the single call succeeding
  IS the proof sys_wait's observable behavior actually changed, since
  the old design could not have worked with only one call.
- **The child branch gained a bounded `sys_nop` spin (200,000
  iterations — the SAME magnitude `kernel/user/hello.asm`'s own
  `LOOP_COUNT` already uses and this codebase already trusts,
  Milestone 17) before it exits**, specifically to make
  `kernel_main`'s new blocking-wait self-test deterministic rather than
  a timing race — see Verification.

## Rejected alternatives
- **A real sleep-queue/wake primitive (`TASK_BLOCKED` + `wake(task)`)**
  — rejected for now: more moving parts than the one motivating caller
  needs, and exactly the generalization a real IPC/synchronization
  milestone should design around a SECOND real use case, not invent
  speculatively for `sys_wait` alone. Flagged in `future.md` as the
  natural next step once that second caller exists.
- **Leave `saved_user_rsp` as a single global and just never let
  `sys_wait` actually re-enable interrupts** (e.g. spin with `IF=0`,
  polling via repeated `hlt`-free busy loops) — rejected: `hlt` with
  `IF=0` would never wake up at all (nothing could ever deliver the
  timer interrupt), and a pure busy-spin with `IF=0` would permanently
  starve every other task including the reaper that's needed to
  produce a match — deadlocks by construction, not a real option.
- **Give every syscall a fixed per-task kernel stack slot for
  `saved_user_rsp` instead of a pointer indirection** (i.e., hardcode
  the field's byte offset into `task_t` and have the asm compute
  `&current_task_ptr[OFFSET]` directly) — rejected: hardcoding a
  struct's byte-offset from assembly is exactly the kind of "wrong bit
  layout, no compile error" landmine CLAUDE.md warns about for
  packed-struct/ABI-sensitive code, and any later reordering of
  `task_t`'s fields would silently corrupt this with no build failure.
  The indirection pointer keeps all field-offset knowledge in C
  (`&current_task->saved_user_rsp`), matching how
  `scheduler_current_pml4`/`scheduler_target_pml4` already keep asm
  dealing only in flat pointers/values, never struct offsets.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-19
  marker unchanged, plus (new) `[OK] blocking wait self-test passed,
  sys_wait genuinely blocked (0xN turns) before the fork demo's child
  exited`, N >= 1 — `kernel_main` PANICS if N is ever 0, i.e. if this
  boot never actually exercised the blocking path at all (an
  all-green board with sys_wait never once genuinely blocking would
  mean this milestone's actual behavior went untested that run).
  `kernel/user/fork_demo.asm`'s child spin (see Decision) makes this
  DETERMINISTIC rather than a race: the child is forced to burn
  through far more of its OWN scheduler turns before it can reach
  `sys_exit` than the parent could possibly be delayed by (the
  parent's own worst-case delay between `sys_fork` returning and
  issuing its `sys_wait` syscall is bounded by the ready queue's own
  small length — one preemption per other task, at most — while the
  child's spin is calibrated far larger than that), so `sys_wait`'s
  FIRST check is guaranteed to find nothing yet, regardless of
  host/QEMU timing. Confirmed empirically: booted 5 times back to
  back, every single boot reported N >= 1 (observed N = 1 or 2 across
  runs — the exact count legitimately varies with host/emulation
  timing, but is never 0).
- `-d int,cpu_reset` trace across a full boot: zero `#PF` (`v=0e`),
  zero double-fault (`v=08`), zero reset events — only IRQ0/IRQ1/IRQ12
  traffic and the pre-existing deliberate `#BP` self-test, confirmed by
  direct grep of the trace file.
- `tests/qemu/test_blocking_wait_selftest.sh` (new): checks the new
  marker, that its reported turn count is nonzero, and that every
  downstream self-test through the shell prompt still passes. All
  nineteen earlier smoke tests and all four host test suites re-run
  and pass unmodified except `test_fork_wait_selftest.sh`'s own header
  comment (updated: "non-blocking" -> the real Milestone 20 behavior;
  its actual assertions were already implementation-agnostic and
  needed no changes).
- Booted 5 times back to back — every boot correct, no flakiness, and
  (per the point above) the new self-test's determinism was itself
  confirmed by this same repeated-boot run, not assumed.

## Known limitations (accepted for this milestone only)
A caller with no children at all blocks forever (see Decision) — same
underlying tracking gap ADR 0018 already accepted, not a new one.
`sys_wait` is still the ONLY blocking syscall; no general
sleep-queue/wake primitive exists yet (see Rejected alternatives) — a
future IPC/synchronization milestone would need one. `target_pid`
(waiting for one SPECIFIC child) is untested by this milestone's own
self-tests, which only exercise the `target_pid == 0` ("any child")
path — `sys_wait`'s implementation handles both identically via
`scheduler_try_wait()` (unchanged from ADR 0018), so this is a test-
coverage gap, not a known code defect.
