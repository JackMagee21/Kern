# ADR 0011: NX (no-execute) enforcement on writable pages

## Status
Accepted and verified — `make run` boots the real ISO, `EFER.NXE` is
enabled before the first NX mapping is created, and a live self-test
confirms the resulting page-table state is actually non-executable
where it should be (the kernel heap) and still executable where it
should be (ordinary kernel code). Landed correctly on the first real
boot, same as ADR 0010.

## Context
Third step of the post-Milestone-8 "build this into an OS" inventory
(memory maturity: "NX enforcement on data pages" was explicitly named
in that list). Every writable region this kernel creates — the kernel
heap, and (since ADR 0009/0010) a ring-3 process's own stack — was, up
to this point, also implicitly executable: nothing set the hardware's
execute-disable bit, so a buffer overflow or a classic stack-smashing
attack that got attacker-controlled bytes into either region could, in
principle, jump into them and run arbitrary code. W^X (a page is never
simultaneously writable and executable) is one of the cheapest, highest
-leverage hardening steps a kernel can take, and this codebase already
had exactly the two writable regions that needed it.

## Decision
- **`vmm_enable_nx()` (kernel/mm/vmm.c) checks CPUID
  `80000001h:EDX` bit 20 before touching `EFER.NXE`**, panicking with a
  clear message if the CPU doesn't support it rather than letting a
  later `VMM_FLAG_NX` mapping fault with a confusing reserved-bit `#PF`
  the first time it's walked. This reuses the exact same extended CPUID
  leaf `boot.asm`'s `check_long_mode` already queries (bit 29, LM) --
  by the time any C code runs at all, leaf `80000000h` having reported
  `>= 80000001h` available is already a proven invariant (`boot.asm`
  would have halted via `panic32` otherwise), so only the NX bit itself
  needed checking here, not the leaf's own availability.
- **Called once, early in `kernel_main`, right after `gdt_init()`/
  `idt_init()` and before `pmm_init()`/`heap_init()`** — must run
  before any `VMM_FLAG_NX` mapping the CPU will ever actually walk for
  a real access exists, and this is the simplest point that
  unconditionally satisfies that without having to reason carefully
  about exact ordering relative to later paging-related setup.
- **`VMM_FLAG_NX` maps directly onto PTE bit 63 (XD)**, following the
  same pattern `VMM_FLAG_WRITABLE`/`VMM_FLAG_USER` already use (a flag
  IS the literal PTE bit, no translation layer). Setting it on ONLY the
  leaf entry is sufficient to block instruction fetch from that page
  regardless of intermediate levels — Intel SDM Vol. 3A Sec. 4.6's
  "most restrictive wins" rule applies to XD the same way it does to
  U/S and writable (if ANY level in the walk has XD=1, execution is
  blocked), and `get_or_create_table` never sets XD on intermediate
  PDPT/PD/PT entries, so this can't accidentally block execution
  anywhere else sharing those intermediate tables.
- **Applied to the two writable regions this kernel actually creates:**
  the kernel heap (`heap_init()`, `kernel/mm/heap.c`) and a ring-3
  process's private stack (`task_create_user()`, `kernel/sched/task.c`)
  — a corrupted heap object or a stack-smashing attack can no longer be
  turned into code execution by jumping into either, regardless of what
  bytes end up there. The demo program's CODE page (read+execute, never
  writable) deliberately does NOT get `VMM_FLAG_NX` — it's the one
  region that's actually supposed to be executable.
- **`vmm_page_is_executable_in()` reads back the real page-table state**
  a mapping produced (present AND XD-clear at the actual leaf,
  correctly handling boot.asm's 2MiB pages too) rather than trusting
  that whatever called `vmm_map_page_in()` got the flag right — used by
  a new `kernel_main` self-test that checks a live heap pointer is
  reported non-executable AND that `kernel_main`'s own code is still
  reported executable (guards against a self-test that would pass
  spuriously if the check function were simply broken and always
  returned false).
- **`exceptions.c`'s `#PF` dump now decodes the error code** (P/W-R/
  U-S/RSVD/I-D bits, Intel SDM Vol. 3A Sec. 4.7 Table 4-12) instead of
  only printing the raw hex value — directly useful for recognizing an
  NX violation (`I/D=1, P=1`) at a glance, and for every future page
  fault this kernel will ever dump.
- **Verification deliberately does NOT trigger a live NX violation and
  watch it fault** — see Rejected alternatives. It checks the resulting
  PTE bit directly instead, which is a real but more indirect proof
  that the plumbing took effect, honestly documented as such rather
  than overclaiming a stronger guarantee than what was actually tested.

## Rejected alternatives
- **Deliberately execute from NX-protected memory (the heap, or a
  process's stack) and confirm the CPU faults, as the live self-test.**
  This is the most direct proof NX actually blocks something, and was
  the first design considered. Rejected for THIS milestone specifically
  because there's no exception-recovery mechanism yet: a fault (unlike
  a trap like `#BP`) resumes AT the faulting instruction, not after it,
  so `isr_handler` returning the same frame unchanged (the `#BP`
  precedent) would just fault again in an infinite loop. Safely
  resuming past a deliberately-triggered fault requires either manually
  computing how far to advance `RIP` (fragile, instruction-encoding-
  dependent, exactly the kind of speculative fix-up CLAUDE.md warns
  against) or a real, general exception-recovery/signal mechanism --
  out of scope here and a large enough feature to deserve its own
  design decision later, not smuggled in as a side effect of adding
  NX. Flagging this rather than building around it quietly.
- **A shared `kernel/arch/x86_64/msr.h`** (new this milestone) for
  `read_msr`/`write_msr`, instead of writing `EFER.NXE`'s access
  directly in `vmm.c` with duplicated logic. `syscall.c` already had
  identical helpers for `STAR`/`LSTAR`/`SFMASK`/`EFER.SCE`; extracting
  them once two real call sites existed is what CLAUDE.md's own "three
  similar lines is better than a premature abstraction" guidance
  implies once there ARE two sites needing the exact same six lines,
  not a hypothetical future one.

## Verification
- `make run` (real toolchain) boots and prints, after `[OK] gdt/idt
  installed`: `[OK] NX (no-execute) enabled`, then later `[OK] NX
  self-test passed (heap is non-executable, kernel code still is)` --
  confirmed by direct `vmm_page_is_executable_in()` checks against a
  live `kmalloc()` pointer (expected non-executable) and `kernel_main`
  itself (expected executable), not eyeballed. Every Milestone 1-10
  marker still prints unchanged afterward, through the shell prompt --
  NX enforcement didn't break the scheduler, syscalls, or process
  lifecycle. Booted 4 times back to back with identical output --
  correct on the first real attempt, no flakiness.
- `tests/qemu/test_nx_selftest.sh` (new): asserts both new markers,
  that NX was enabled strictly before the kernel heap was mapped (real
  ordering, extracted from actual output line numbers, not assumed),
  and that the shell still starts normally afterward.
- All ten earlier smoke tests (through ADR 0010's process-lifecycle
  test) re-run and pass. All three host tests re-run and pass.
