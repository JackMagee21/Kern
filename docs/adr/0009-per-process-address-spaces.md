# ADR 0009: Per-process address spaces

## Status
Accepted and verified — `make run` boots the real ISO, two independent
ring-3 processes get genuinely distinct top-level page tables, and both
run correctly (including making validated syscalls) without interfering
with each other. Two real bugs were found via actual boots, not review
— see Verification for the full diagnostic trail.

## Context
Through Milestone 7, every task — kernel threads and the one ring-3
demo task alike — shared a single page-table hierarchy; `CR3` was never
reloaded. That's not real process isolation: two ring-3 programs at the
same virtual address would collide. This is the first of the "what
would it take to build this into a real OS" list discussed with the
user, and the one everything else on that list depends on (a
filesystem needs somewhere to load a program's isolated memory into; a
real process model needs processes that can't corrupt each other).

## Decision

- **Kernel threads keep sharing the kernel's own original address
  space; only ring-3 processes get a private one.** `task_create()`
  (kernel threads) just records `vmm_current_pml4()` at creation time —
  they're part of the kernel, not isolated/untrusted, so there's no
  reason to give them their own tables. Only `task_create_user()` calls
  `vmm_create_address_space()`.
- **A new process's table shares two entries with the kernel's, copied
  by reference (the PML4 entry itself — a pointer to the next-level
  table — not a snapshot of its contents):** `PML4[511]` (kernel image
  + heap, already the shared region since ADR 0004/0007) and,
  necessarily (see Verification), `PML4[0]` (boot.asm's low identity
  map). Copying the *entry* rather than cloning the subtree means any
  later kernel heap growth, or anything else added under either shared
  entry, is automatically visible to every existing process's table
  too, forever — not just a point-in-time snapshot.
- **Sharing the identity map is safe because it stays supervisor-only.**
  Every identity-mapped entry boot.asm ever created has `U=0`; copying
  the PML4 entry into a process's table doesn't change that at any
  leaf. This grants kernel *code* (running under that process's `CR3`
  during a syscall or exception — see below for why that happens)
  broader reach; it grants ring-3 code in that process nothing new,
  since the permission check that actually matters for ring-3 access is
  still the leaf-level `U` bit, unchanged.
- **Process code/stack deliberately live under PML4 index 1
  (`0x8000000000`+), not index 0.** `PML4[0]` is now committed entirely
  to the shared identity map (see above), so process-private mappings
  can't live in that same slot — moved from the address ADR 0007
  originally used (`0x400000`, under index 0) to the same offset
  relocated into index 1 (`0x8000400000`). Both processes still use
  identical addresses, safely, since each has its own independent table
  beneath that slot.
- **`vmm_map_page_in(pml4_phys, ...)`, `vmm_current_pml4()`, and
  `vmm_create_address_space()` were added as the minimum API surface
  needed** to build a new process's tables while the kernel's own
  address space stays active throughout (required for the identity-
  mapping bootstrap trick — see ADR 0004 — to keep working: every table
  frame a process's setup allocates must still be reachable via the
  *caller's* live identity map). `vmm_map_page()` is now defined as
  `vmm_map_page_in(vmm_current_pml4(), ...)`.
- **The demo program's physical code page is shared read-only across
  every process**, not copied — safe since it's never mapped writable,
  the same way real OSes share program text between instances of one
  program. Only the stack is private per process (fresh
  `pmm_alloc_frame()`s each call).
- **`CR3` is reloaded by the scheduler on every switch, but the actual
  write happens in assembly (`common_stub.inc`), not in the C handler
  — this is the more serious of the two bugs found, and is discussed in
  full under Verification** rather than restated here, since the "why"
  only makes sense alongside the diagnostic trail that found it.
- **Two ring-3 processes in the self-test, not one.** Milestone 7's
  single-process test could pass even with a subtly broken isolation
  boundary (nothing else exists to collide with). Running two —
  printing both their `PML4` physical addresses, and asserting the
  "hello from ring 3" message appears exactly twice, once per process —
  is the actual proof of the property this ADR is about.

## Rejected alternatives
- **Give kernel threads their own address spaces too** — no benefit
  identified; they're trusted, kernel-internal code, not isolated
  workloads. Would only add CR3-reload overhead for no isolation gain.
- **Copy the kernel-half subtree into each process instead of sharing
  the PML4 entry** — would desync from future kernel/heap growth
  immediately, defeating the point; rejected without much debate.
- **Keep process code at `0x400000` (PML4 index 0) and don't share the
  identity map** — this was the first attempt, and it's what caused the
  second bug (see Verification): kernel code running under a process's
  `CR3` needs identity-mapped structures reachable, and there's no way
  to provide that without either sharing `PML4[0]` or giving every
  process its own private copy of the identity map (wasteful, and still
  needs the SAME "which PML4 slot is process-private" conflict resolved
  first).

## Verification

**Bug 1: CR3 switched while still running on the outgoing task's
stack.** First real boot triple-faulted immediately after the first
tick that should have switched into a process. Diagnosed with
`-d int,cpu_reset` (CLAUDE.md's explicit prescription for exactly this
situation) rather than guessing: the trace showed a `#PF` at a kernel
address with `CR3` already reloaded to the incoming process's table but
`RSP` still pointing into bootstrap's low `boot_stack` — which that
process's table doesn't map (no identity mapping at the time). Root
cause: `timer_tick_handler`'s C code executed `mov cr3, ...` and then
kept running (finishing its own C function body, its epilogue, the
`return`) *while still on the outgoing task's stack* — and the very
next stack access after the switch is exactly what faulted. Fixed by
moving the actual `mov cr3` into `common_stub.inc`'s assembly, placed
*after* `mov rsp, rax` — i.e., after `RSP` has already moved to the
incoming task's own saved-frame location, which (for every task, kernel
thread or process) lives somewhere reachable regardless of which `CR3`
was active a moment before. Verified by disassembly that the generated
code encodes in that order, then a clean boot with the fix.

**Bug 2: kernel code needs identity-mapped structures regardless of
which process's `CR3` is active.** Second boot got further (no triple
fault) but produced garbled output — alternating bare `\n` and `[`
characters. Traced to: `sys_write`'s `console_putc()` writes straight
to `0xB8000`, which isn't mapped under a process's table; the resulting
`#PF` then entered `isr_handler`, which *also* calls `console_write()`
as part of reporting the fault, which *also* faults on the same VGA
access — nesting indefinitely, with only the first character of each
nested attempt's string reaching serial before the next fault, which
exactly matches the observed byte pattern. First fix attempt
(`vga_set_buffer()`, a dedicated high mapping just for VGA) worked but
only treated the symptom — `vmm.c`'s own `get_pml4()`/`is_user_page()`
(used by `vmm_is_user_range()`, called from *every* syscall that
touches a user pointer) have the identical problem, just not yet hit.
Found this before shipping the narrow fix by reasoning through what
else relies on the same assumption, not by waiting for a third crash.
Replaced with the general fix: share the identity map itself (see
Decision) — verified via `-d int,cpu_reset` that the exact same
scenario (register state showed `CR2` equal to a process's own `PML4`
physical address, from `vmm.c` dereferencing the live `CR3` value
directly) went away, then a clean boot, then three repeat boots with no
flakiness.

**Final verification:**
- `make run` (real toolchain) boots and prints all Milestones 1-8
  markers unchanged, then `[OK] process A pml4: 0x23b000, process B
  pml4: 0x244000 (different address spaces)`, then **two** independent
  `[OK] hello from ring 3 via syscall` lines, then all self-tests
  (timer/scheduler/syscall) passing with roughly double the syscall
  count of the single-process Milestone 7 run (both processes' `sys_nop`
  loops contributing).
- `tests/qemu/test_process_isolation_selftest.sh` (new): extracts both
  processes' `PML4` addresses from real output and asserts they differ,
  and asserts the "hello from ring 3" message appears exactly twice.
- All eight earlier smoke tests re-run and pass (two needed their
  expected marker text updated after `kernel_main`'s log lines changed
  to describe two processes instead of one — test-text fixes, not
  behavior regressions, same pattern as every previous milestone
  transition). All three host tests re-run and pass.
- Booted 3 additional times back to back with no flakiness observed.
