# ADR 0022: `sys_exec`

## Status
Accepted and verified -- `make run` boots the real ISO, a new self-test
(`syscall_get_exec_count()`) confirms `sys_exec` actually resolved into
a real image swap at least once, and `kernel/user/exec_target.asm`'s own
message appearing (strictly after `exec_demo.asm`'s pre-exec message)
proves a genuinely different program's code ran. Correct on the first
real boot attempt; booted 5 times back to back with identical shape
(process reap ordering among independent tasks varies run to run, as it
already did before this milestone, but the exec-specific sequence and
every count are identical every time). `-d int,cpu_reset` trace across a
full boot: still exactly 3 `#PF` events (unchanged from Milestone 21 --
`sys_exec`'s own address-space reset never faults, see Decision below),
zero double-fault/reset events. All twenty-one pre-existing smoke tests
and all four host test suites re-verified passing, including the
frame-leak count in `test_process_lifecycle_selftest.sh` (now updated
from 4 to 5 reaps, still landing on the exact same pre-creation baseline
-- proving the mid-flight address-space reset-and-repopulate cycle
leaks nothing).

## Context
`future.md` flagged `sys_exec` as the natural next item after Milestone
18's `sys_fork` -- the other half of the fork/exec pair every real OS
that has fork also has. It also flagged this as GENUINELY HARDER than
another syscall, reasoning (before this milestone started) that
`sys_exec` "can never resume through the normal `sysretq` epilogue
`syscall_entry.asm` always takes, because the OLD program (and its
stack) is gone" and would need "a genuinely NEW control-flow primitive,
a synchronous mid-syscall resume via `iretq`."

That reasoning turned out to be wrong, and re-reading `syscall_entry.asm`
and `syscall.h`'s `syscall_frame_t` doc comment before writing any code
is what found the mistake: `frame->rcx`/`frame->r11` (the fields
`syscall_entry.asm`'s exit path pops and hands to `sysretq` as the
return RIP/RFLAGS) are just ordinary fields of the `syscall_frame_t` a
syscall handler already has direct, unrestricted write access to --
`sys_fork` already overwrites `frame->rax` on the normal return path,
and nothing about the assembly cares WHAT values are in `rcx`/`r11` when
it pops them, only that they're present. There is no hidden assumption
anywhere in `syscall_entry.asm` that ties the resumed RIP to "the same
program this syscall was made from" -- it's just data on a struct.

## Decision

- **No new control-flow primitive.** `sys_exec` resumes through the
  EXACT SAME `sysretq` epilogue every other syscall already uses.
  `task_exec()` (`kernel/sched/task.c`) overwrites the CURRENT syscall's
  own `syscall_frame_t` in place: `rcx` = the new image's `e_entry`,
  `r11` = a fresh RFLAGS (`0x202`, IF=1), every OTHER field (via
  designated-initializer default, including `rax`) zeroed -- a fresh
  program shouldn't see the old image's leftover register state, the
  same "don't leak prior context" reasoning a real `exec()`'s register
  reset follows. `syscall_entry.asm` was not touched at all this
  milestone -- the FIRST syscall since Milestone 7 to add genuinely new
  process-image-replacing behavior without needing any assembly change.
- **Same `task_t`, same pid, same PML4 frame -- never a new process.**
  This is `sys_exec`'s defining property, the thing that distinguishes
  it from `sys_fork` + `sys_exit`. `task_exec(task_t *task, ...)` takes
  the CALLING task directly (`scheduler_current_task()`, same pattern
  `sys_wait` already uses) and mutates its EXISTING `pml4` field's
  contents and its EXISTING `saved_user_rsp` field's value -- never
  calls `vmm_create_address_space()`, `kmalloc()`s no new `task_t`,
  never touches `next`/`prev`/`id`/`parent_id`. Verified by
  `kernel_main`'s own reap-count accounting (`scheduler_reaped_count()`,
  raised from 4 to 5, not 6): if `sys_exec` had instead silently created
  a NEW task while abandoning the old one, this count would visibly be
  wrong (either 6, if both somehow exited, or permanently stuck below 5,
  if the old one never resumed and hung) -- a stronger, kernel-side proof
  than any userspace pid-comparison convention could give without
  inventing a fragile cross-image argument-passing ABI this milestone
  doesn't need.
- **`vmm_reset_user_address_space()` (`kernel/mm/vmm.c`), a new sibling
  of `vmm_destroy_address_space()` with the OPPOSITE activity
  requirement.** `vmm_destroy_address_space()`'s documented contract
  requires its target NOT be the currently active address space (ADR
  0010's whole reason the reaper exists, not the exiting process
  itself). `sys_exec` is the first caller that needs to reset a process's
  OWN address space while it's still genuinely active (`CR3` unchanged
  throughout -- `SYSCALL` never switches it, the same invariant
  `task_fork()` already relies on for reading the parent's own pages
  directly). Both functions now share one factored-out walk
  (`free_process_private_frames()`, a pure refactor of the loop
  `vmm_destroy_address_space()` already had -- verified behavior-
  identical for the destroy path by re-running every test that exercises
  it) that additionally `invlpg`s each leaf's own reconstructed virtual
  address as it's cleared. This is what makes running it against the
  ACTIVE address space safe: any translation the TLB was caching for
  that exact VA is invalidated in the same pass that clears the PTE, so
  nothing stale is left over for `elf_load()`'s subsequent
  `vmm_map_page_in()` calls to silently be shadowed by. (For the
  DESTROY path, where the target is guaranteed inactive, this `invlpg`
  is a harmless no-op -- the CR3 switch away from that address space,
  which the reaper's caller already performed before destroy ever runs,
  already flushed anything that would have been stale.) A full CR3
  self-reload was considered and rejected in favor of this -- see
  Rejected alternatives.
- **`task_exec()` validates the requested image BEFORE tearing anything
  down.** `exec_lookup_image()` (a small fixed table -- no filesystem
  exists to load an arbitrary path from, the same closed-world
  constraint `task_create_user()` already accepts) is checked first; a
  bad `program_id` returns `false` with the process's address space
  completely untouched, so `sys_exec` can report `-1` in `rax` and let
  the OLD image keep running normally -- a real, expected bad-input
  outcome (CLAUDE.md's parser-security stance), not a kernel bug or a
  half-torn-down process left behind.
- **Two new, genuinely distinct embedded programs
  (`kernel/user/exec_demo.asm`, `kernel/user/exec_target.asm`), not a
  reuse of `hello.elf`/`fork_demo.elf`.** Both of those existing images'
  OWN self-tests count their own messages an EXACT number of times
  (`kernel_main`, `test_elf_loader_selftest.sh`) -- execing into either
  would have silently perturbed those counts instead of proving
  anything new about `sys_exec` itself. `exec_target.asm`'s message and
  exit code (`0x37`) are both unique to it, so its appearance in the
  boot log unambiguously proves a real code swap, not a coincidental
  substring match.

## Rejected alternatives
- **A synchronous mid-syscall `iretq` resume, as `future.md` originally
  scoped this.** Rejected once shown unnecessary (see Context) --
  reusing the existing `sysretq` epilogue is strictly simpler, touches
  zero assembly, and there is no correctness gap it leaves open: the
  epilogue's job was always "restore GPRs from this frame, resume at
  `rcx`/`r11`/[the per-task RSP]," and `task_exec()` supplying DIFFERENT
  values for those same fields is not a special case the assembly needs
  to know about.
- **A full CR3 self-reload (`mov cr3, cr3`) instead of per-leaf
  `invlpg`** inside `vmm_reset_user_address_space()`. Would also
  correctly flush every stale entry in one shot, and IS distinguishable
  from ADR 0009's documented CR3-switch-timing hazard (no RSP switch is
  involved, no scheduler state goes out of sync, and the physical
  address loaded is bit-for-bit identical to what CR3 already holds --
  its only effect is the flush). Rejected anyway in favor of per-page
  `invlpg`: every other mutator in this codebase (`vmm_map_page_in()`,
  `vmm_unmap_page()`) already uses the granular, per-page idiom, and
  introducing the codebase's first-ever explicit CR3 write for this one
  call site would be a new pattern for no correctness benefit the
  existing idiom doesn't already provide.
- **A `SYS_GETPID` syscall, to let `exec_demo.asm`/`exec_target.asm`
  print and cross-check their own pid across the exec.** Rejected as
  unnecessary scope: `kernel_main`'s reap-count accounting already
  proves the stronger "same process" property (see Decision), and a
  cross-image register/stack passing convention to carry a pid value
  across an image swap that zeroes every GPR would be a real, fragile
  new ABI this milestone doesn't need to invent. Considered and dropped
  before implementation, not built then removed.
- **Reusing `hello.elf` as the exec target.** Rejected -- see Decision;
  would have silently broken `test_elf_loader_selftest.sh`'s exact
  message-count assertions for an unrelated milestone.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-21
  marker unchanged, plus (new) `[OK] exec demo process created, pid
  0x7` -> `[OK] exec demo running, about to sys_exec into a new image`
  -> `[OK] exec target running -- process image was genuinely replaced
  by sys_exec` -> (later) `[OK] process 0x7 exited and was reaped`
  (exactly once, same pid) -> `[OK] exec self-test passed, sys_exec
  replaced a running process's image 0x1 time(s) without creating a new
  process`. `kernel_main` PANICs if `syscall_get_exec_count()` is ever
  0. Booted 5 times back to back -- the exec-specific sequence and every
  count identical every time (inter-process reap ORDERING among the
  five independent processes varies run to run, as it already did
  before this milestone -- not a new source of flakiness).
- `-d int,cpu_reset` trace across a full boot: still exactly 3 `#PF`
  (`v=0e`) events, matching Milestone 21's own count exactly -- `sys_exec`
  added ZERO new faults, confirming the address-space reset-and-
  repopulate cycle happens entirely through explicit, kernel-mediated
  `vmm_map_page_in()`/`invlpg` calls rather than ever relying on fault-
  driven recovery. Zero double-fault (`v=08`), zero reset events.
- `tests/qemu/test_exec_selftest.sh` (new): checks all four new markers,
  absence of the demo's own `[FAIL]` control-flow marker, real
  sequencing (target's message strictly after the demo's own pre-exec
  message, not a coincidental substring match), the exact reap count
  (5, not 6 -- the strongest proof `sys_exec` reused its caller's own
  process), and the frame-leak self-test. `test_fork_wait_selftest.sh`
  and `test_process_lifecycle_selftest.sh` both needed their exact
  reaped-count assertion updated from 4 to 5 (scope growth -- a fifth
  process now exists -- not a behavior fix); their header comments
  updated to match. All twenty-one earlier smoke tests and all four host
  test suites re-verified passing.

## Known limitations (accepted for this milestone only)
`sys_exec` can only target one of a small, fixed, build-time-embedded
set of images (currently one: `exec_target.asm`) -- no filesystem exists
to load an arbitrary path, the same closed-world constraint
`task_create_user()` already accepts for process creation generally; a
real `execve`-style path-based lookup is future work contingent on a
real FS (a flagged non-goal pending the user's decision). No
argv/envp-equivalent is passed across the image swap -- every GPR
(including any the caller might have wanted to hand the new program) is
deliberately zeroed, matching this kernel's existing "programs take no
arguments" convention (`hello.asm`/`fork_demo.asm` don't either). A bad
`program_id` is the only validated-input failure path; an ELF64
validation failure on the (build-time-trusted) embedded target image
still panics rather than returning `-1`, the same stance
`task_create_user_image()` already takes for its own embedded image.
