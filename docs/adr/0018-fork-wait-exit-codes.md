# ADR 0018: `sys_fork`, non-blocking `sys_wait`, and exit codes

## Status
Accepted and verified — `make run` boots the real ISO, a ring-3 process
forks, the child runs as a genuinely independent process (its own
deep-copied address space, not aliasing the parent), and the parent's
non-blocking `sys_wait` correctly observes the child's real exit code.
Correct on the first real boot attempt — no live-diagnosis bug this
time, same as Milestone 17. All eighteen earlier smoke tests and all
four host test suites re-verified passing.

## Context
`future.md`'s "reasonable next steps" named process lifecycle maturity
next: Milestone 10 built `sys_exit` and teardown but explicitly deferred
`fork`/`exec`-equivalent syscalls and a parent/child relationship,
since nothing had a parent-process concept at all (every process was
spawned directly by `kernel_main`). Milestone 17's ELF loader
(`elf_load()`) is what makes an `exec`-shaped feature meaningful to
build toward; `fork` is the more foundational half of that pair (a
process needs a way to become two processes before "replace my own
image" is interesting), so it came first.

## Decision

- **Deep-copy fork, not copy-on-write.** `task_fork()`
  (`kernel/sched/task.c`) walks the parent's ENTIRE process-private
  region (`vmm_for_each_user_page()`, a new read-only enumeration added
  to `vmm.c` alongside `vmm_destroy_address_space()`'s existing
  traversal) and, for every present page, allocates a fresh frame,
  copies its bytes, and maps it at the SAME virtual address in a brand
  new address space. This kernel has no page-fault-driven allocation
  path at all yet (every mapping through Milestone 17 was eagerly
  created, never faulted in) — building real COW would mean building
  that mechanism first, which is exactly the kind of half-finished
  implementation CLAUDE.md warns against attempting inline with an
  unrelated feature. Flagged as the natural next memory-maturity item,
  not silently assumed unreachable.
- **Fork reads the parent's pages through ordinary virtual addresses,
  not by dereferencing their physical frames directly.** This works
  specifically because `task_fork()` is only ever reached from
  `sys_fork`, itself only ever reached MID-SYSCALL — and SYSCALL/SYSRET
  never switches `CR3` (ADR 0007), so the parent's own mappings are
  still the live, currently-active ones for the entire duration of the
  fork. This sidesteps needing the SOURCE side to fall within
  `VMM_IDENTITY_WINDOW_LIMIT` at all (only the DESTINATION frame does,
  same constraint the ELF loader already has, ADR 0017) — a real
  simplification, not just an implementation convenience, since a
  parent process's pages could otherwise live anywhere in physical
  memory by the time it forks.
- **The child's initial resume context is a synthetic `trap_frame_t`
  built from the parent's `syscall_frame_t` at the moment of the fork,
  not from a `trap_frame_t`/iretq-style saved context.** Syscalls in
  this kernel don't go through the IDT (ADR 0007) — there is no
  `trap_frame_t` for the currently-forking process at all at this
  point, only the narrower `syscall_frame_t` `syscall_dispatch()`
  already has, plus the piece it's missing: the user's RSP, which
  `syscall_entry.asm` had been keeping in a file-local `saved_user_rsp`
  purely to restore it before `sysretq`. That static had to be promoted
  to a `global` symbol (`syscall_get_user_rsp()`) — the one real "new
  primitive needed" this milestone required, not something avoidable by
  restructuring `task_fork()` differently. Field-by-field, the two
  register structs share every GPR NAME (`trap_frame_t` is a superset:
  same GPRs plus `vector`/`error_code`/`rip`/`cs`/`rflags`/`rsp`/`ss`),
  so building the synthetic frame is a direct, self-documenting
  designated-initializer copy by name, with exactly one deliberate
  deviation: `rax` is forced to `0` (fork's "you are the child" signal)
  instead of copied — every other field, including the R11 slot that
  doubles as both a GPR value and (separately, in `.rflags`) the actual
  flags register to restore, reflects real x86-64 SYSCALL/SYSRET ABI
  behavior (SYSCALL itself clobbers user R11 with the pre-syscall
  RFLAGS; a normal, non-forked SYSRET round trip restores BOTH the R11
  GPR and RFLAGS from that same value) — not a special case invented
  for this feature.
- **`sys_wait` is NON-blocking, by necessity, not by choice.** Syscalls
  in this kernel are non-preemptible and run with interrupts masked for
  their entire duration (`SFMASK`, ADR 0007's known limitation, still
  true) — a genuinely blocking wait would need to sleep with interrupts
  enabled and a way for something else to wake the caller back up,
  neither of which exists (no blocking/sleep-queue primitive at all).
  Building one now would be exactly the synchronization/IPC milestone
  `future.md` already lists as separate, unstarted future work — not
  something to improvise as a side effect of `sys_wait`. Instead,
  `sys_wait` reports "no matching exited child yet" (return value `0`,
  never a valid pid) and the CALLER polls — `kernel/user/fork_demo.asm`'s
  parent branch loops `sys_wait` with a short `sys_nop` spin between
  attempts, the same "wait for an async event" pattern this kernel
  already uses everywhere (`kernel_main`'s `hlt`-until-a-counter-
  advances self-tests), just issued from ring 3 via repeated syscalls
  instead of from kernel code directly.
- **A parent/child relationship needs the reaper to stop unconditionally
  freeing every `task_t` it processes.** `task_t` gained `parent_id`
  (`0` = orphan, the sentinel every pre-existing call site already
  produces since real ids start at 1) and `exit_code` (set by
  `scheduler_exit_current()`, now parameterized, right before a task
  becomes `TASK_ZOMBIE`). The reaper still reclaims a zombie's
  RESOURCES (address space, both stacks) unconditionally and
  immediately, exactly as ADR 0010 already established — only the
  `task_t` struct itself's lifetime changed: an orphan is `kfree()`'d
  right away (unchanged behavior), but a task with a parent is pushed
  onto a NEW chain (`collected_head`) instead, reusing `task_t::next` a
  THIRD time (ready queue → zombie-hand-off chain → this one; a task is
  only ever on one of the three at once). `scheduler_try_wait()`
  scans/unlinks/frees from that chain. This deliberately does NOT
  change what `scheduler_reaped_count()` measures (RESOURCE reclaiming,
  what the existing frame-leak self-test cares about) — a task with a
  parent that's never `wait()`-ed for still counts as "reaped" the
  moment its memory comes back; only its small `kmalloc`'d `task_t`
  struct (not tracked by any existing self-test) would leak, a known,
  accepted limitation (see below), not a regression of the thing
  Milestone 10 actually proved.
- **`collected_head` needs `cli`/`sti` on BOTH sides, unlike
  `zombie_head`.** `zombie_head`'s producer (`timer_tick_handler`) runs
  in interrupt context, inherently atomic w.r.t. itself on one CPU, so
  only its (normal-context) consumer needs a critical section.
  `collected_head`'s producer is the reaper task itself — an ordinary,
  PREEMPTIBLE kernel thread — so its own push could be interrupted
  mid-update by an unrelated timer tick; wrapping it in `cli`/`sti` too
  (matching this codebase's existing idiom, generalized per CLAUDE.md
  safety rule 1 from "shared with an interrupt handler" to "shared with
  the only other context that could concurrently run on this one CPU")
  closes that gap. `sys_wait`'s own side is already atomic by
  construction (a syscall's entire duration runs with `IF=0`), so its
  `cli`/`sti` is defense-in-depth/consistency with the pattern, not
  strictly load-bearing on that side alone.
- **The fork/wait demo (`kernel/user/fork_demo.asm`) is a THIRD
  `kernel_main`-spawned orphan process, additive alongside the existing
  two `hello.asm` processes, not a replacement for them.** Keeps
  Milestone 17's exact, already-tested log lines/marker text unchanged
  (new behavior gets its own new log line) rather than folding new
  scope into an existing assertion. `kernel_main`'s existing frame-leak
  self-test threshold moved from `scheduler_reaped_count() < 2` to `< 4`
  (2 hello processes + the fork-demo parent + its runtime-forked child)
  since `frames_before_processes` is captured before all of them now.

## Rejected alternatives
- **Copy-on-write fork** — rejected for this milestone; no
  page-fault-driven allocation mechanism exists to build it on top of
  (see Decision). Flagged as a real future step, not silently assumed
  away.
- **A blocking `sys_wait`** — rejected; would require a real
  blocking/sleep-queue scheduler primitive this kernel doesn't have,
  itself a separate, larger, already-flagged future milestone
  (synchronization/IPC). A non-blocking, poll-based design is an
  honest, working subset, not a shortcut disguised as the real thing.
- **Reparenting an exited process's own unwaited children to some
  fallback (e.g. the bootstrap task), so nothing is ever permanently
  abandoned** — real Unix systems do something like this (init
  inherits orphans); rejected here as unneeded complexity for a kernel
  where nothing currently exercises a parent exiting before its child
  (`fork_demo.asm`'s parent always outlives its one child in every
  actual code path this milestone has). Flagged as a known limitation
  instead of silently building unreachable machinery for it.

## Verification
- `make run` (real toolchain) boots and prints, after all Milestones
  1-17 markers unchanged: `[OK] fork/wait demo process created, pid
  0x...`, then (before the two `hello` processes' own messages, in this
  particular run — scheduling order isn't guaranteed, and the test
  doesn't assume one) `[OK] child process running after fork`, then
  later `[OK] fork/wait self-test: child exit code verified`, then four
  `[OK] process N exited and was reaped` lines (the child specifically
  reaped before its still-polling parent, correctly reflecting that it
  exits first), then the existing timer/scheduler/syscall self-tests,
  then `[OK] process lifecycle self-test passed, ... 0x7d90 frames
  free, matches pre-creation baseline` — an EXACT match, proving the
  child's entire deep-copied address space (private frames, not shared
  with the parent) came back fully, not just "close enough."
  `tests/qemu/test_fork_wait_selftest.sh` (new) independently checks
  real sequencing (child message after process creation), the exact
  exit-code-verified message, absence of its `[FAIL]` counterpart, and
  the four-reaps/leak-free combination.
  `tests/qemu/test_process_lifecycle_selftest.sh` needed its exact
  reaped-count assertion updated from 2 to 4 (a scope-growth marker
  update, not a behavior fix, same pattern as every previous milestone
  transition).
- All eighteen earlier smoke tests and all three pre-existing host test
  suites re-run and pass unmodified beyond that one marker-count update.
- Booted 4 times back to back with identical shape each time (4/4
  reaps, 0 `[FAIL]` lines, exact frame-count match) — correct on the
  first real attempt, no flakiness.

## Known limitations (accepted for this milestone only)
No copy-on-write (every fork is a full, eager deep copy — real but
memory-costly for a large process, acceptable at this kernel's current
scale). No blocking `wait()` (poll-based only, see Decision). No
`exec`-equivalent syscall yet (a forked child always runs the exact
same image as its parent; replacing a running process's image with a
DIFFERENT embedded program via a new syscall is the natural next step
now that both `elf_load()`, Milestone 17, and a real child process to
call it from, this milestone, both exist). No reparenting of orphaned
children (an exited parent's own never-`wait()`-ed-for child's `task_t`
struct — not its memory, which the reaper already reclaims
unconditionally — would never be freed; not exercised by anything in
this codebase today, see Rejected alternatives). No process groups,
signals, or `SIGCHLD`-equivalent notification — `sys_wait`'s polling
model is the entire "how does a parent learn about its child" story for
now.
