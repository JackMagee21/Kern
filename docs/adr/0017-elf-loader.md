# ADR 0017: ELF64 loader for ring-3 processes

## Status
Accepted and verified — `make run` boots the real ISO, both ring-3
processes each parse and map their own private copy of a real compiled
ELF64 executable (not Milestone 7-16's hand-written raw code blob), and
the loaded program's own multi-segment self-check (`.data` initializer
present, `.bss` genuinely zeroed) passes for both. Correct on the first
real boot attempt (no bugs needed live diagnosis this time — see
Verification). All sixteen earlier smoke tests and all four host test
suites re-verified passing.

## Context
`future.md`'s "reasonable next steps" (post-Milestone-16, not adjacent
to any flagged non-goal) named an ELF loader first: every ring-3
process through Milestone 16 ran the exact same hand-written NASM blob
(`kernel/sched/user_demo.asm`), mapped as one shared, read-only,
always-executable code page. That's not a loader — it's a single
hardcoded program. A real ELF64 loader is the natural next step toward
running more than one distinct program, and — per CLAUDE.md's non-goal
list — doesn't need a filesystem to do it: the ELF image can still be
embedded/statically linked into the kernel image itself, exactly the
way `user_demo.asm` already was.

## Decision

- **Two layers, split at the same boundary every other subsystem in
  this kernel uses: a pure, host-testable parser (`libk/elf.h/.c`) and
  a thin kernel-only mapper (`kernel/mm/elf_loader.c`).** The parser
  knows the ELF64 spec's byte layout and does every bounds/overflow
  check CLAUDE.md's security rule requires ("validate all sizes/offsets
  in any parser... before use") — it has no allocator, no VMM, no
  kernel dependency at all, so `tests/host/test_elf.c` exercises it with
  ASan/UBSan the same way `test_fmt.c`/`test_heap_alloc.c` do. The
  mapper only ever calls the parser's already-validated accessors before
  touching `pmm`/`vmm`.
- **Per-segment W^X, derived from the ELF's own `p_flags`.**
  `VMM_FLAG_WRITABLE` iff `PF_W`, `VMM_FLAG_NX` iff `!PF_X`. Strictly
  more precise than the old demo blob's fixed policy (one page, always
  R+X, never writable) — this is a genuine security improvement, not
  just a refactor, and it's what the embedded program's own multi-page
  `.data`/`.bss` segment (writable, non-executable) needed to work at
  all.
- **Every loaded segment's destination frames are `VMM_FLAG_OWNED` and
  allocated FRESH per process — no shared/copy-on-write text pages.**
  This is the one real regression versus the old design: Milestone
  7-16's demo code page was shared read-only across every process (one
  physical page, N processes). A real ELF loader's segments are
  correctly *marked* private per this decision, so two processes running
  the same binary now duplicate its physical memory instead of sharing
  it. Accepted deliberately: the memory-maturity list already flags
  demand paging/COW as unstarted future work (ADR 0004/0009's known
  limitations); doing it now would mean guessing at a COW design before
  anything requires it. `VMM_FLAG_OWNED` is not optional either way —
  since these frames ARE process-private now (unlike the shared demo
  page, which deliberately omitted the flag), omitting it here would
  silently leak physical memory on every process exit; the process
  lifecycle self-test (Milestone 10) catches this class of bug by
  design, and did pass.
- **Simplifying assumption: every `PT_LOAD` segment's `p_vaddr` must be
  4KiB-page-aligned** (`libk/elf.h`'s `elf64_validate_load_segment`,
  enforced as a hard rejection, not a silent truncation). True for any
  image `kernel/user/user.ld` produces (explicit per-section
  `ALIGN(4K)`), not true in general for an arbitrary ELF file, which may
  pack multiple segments' boundaries within one shared page for
  file-size efficiency. Handling that would mean mapping a single
  physical page with permissions that satisfy TWO different segments'
  `p_flags` simultaneously (or splitting the page) — real complexity
  with no current caller that needs it, since this loader only ever
  loads images this repo's own `user.ld` produces. Flagged here, not
  silently assumed away, per CLAUDE.md's "no half-finished
  implementations" combined with "validate... before use": a
  non-page-aligned segment is REJECTED (`elf_load` returns false), never
  silently mis-mapped.
- **Destination frames must fall within `VMM_IDENTITY_WINDOW_LIMIT`**
  (moved from `vmm.c` into `vmm.h` this milestone, since `elf_loader.c`
  is now a second caller of the same constant) — the loader needs to
  write segment bytes into a freshly allocated physical frame BEFORE
  it's mapped anywhere, which only works by dereferencing the frame's
  own physical address directly, the same bootstrap trick `vmm.c`'s
  page-table allocator already relies on (ADR 0004's known limitation:
  no general physical-memory direct-map exists yet). Panics, doesn't
  silently corrupt memory, if ever violated — same discipline as every
  other identity-window check in this codebase. In practice this holds
  comfortably: `pmm_alloc_frame()` hands out the lowest-numbered free
  frame first, and this milestone's total per-process footprint (4
  segment frames for the embedded demo) is a tiny fraction of the 8MiB
  window even after everything earlier in boot has already allocated
  from it.
- **`user_demo.asm` retired outright, not kept alongside the new
  loader.** Maintaining two parallel "how does ring-3 code get into a
  process" mechanisms would just be dead weight once the ELF path
  covers everything the old one did (and more — it's the first design
  that actually exercises a writable, `.bss`-bearing segment). Its
  `boot/linker.ld` section, the `user_demo_start_lma`/`_end_lma`
  symbols, and the file itself are all removed in this change, not
  deprecated in place.
- **The embedded ELF image is built as a genuinely separate link,
  not folded into the kernel's own build.** `kernel/user/hello.asm` +
  `kernel/user/user.ld` are assembled and linked (`x86_64-elf-ld -T`,
  not `x86_64-elf-gcc`) with NONE of the kernel's flags
  (`-mcmodel=kernel`, `-mno-red-zone`, etc. — irrelevant/wrong for
  ordinary ring-3 user code) into a real standalone
  `build/kernel/user/hello.elf`. `kernel/sched/user_elf_blob.asm` then
  `incbin`s that file's raw bytes into the kernel image's own `.rodata`
  as an explicit Makefile dependency (the blob file must exist before
  that `.asm` is assembled) — this is what lets `elf_loader.c` parse a
  REAL ELF64 file, header and all, rather than a synthetic
  representation of one.
- **The demo program itself was redesigned, not just re-linked, to
  actually exercise the new loader's capabilities**
  (`kernel/user/hello.asm`): a `.data` segment with a real non-zero
  initializer and a `.bss` segment, checked at runtime (read `bss_var`,
  must be 0; read `data_var`, must be its real `0x1234` initializer;
  write `bss_var`, proving the page is genuinely writable) before
  proceeding to the existing `sys_nop`-loop/`sys_exit` self-test every
  process already did. A failure prints a distinctly different message
  (`[FAIL] elf .data/.bss segment verification failed`) the new smoke
  test explicitly checks is ABSENT, not just that the success message
  is present — a loader bug that left `.bss` un-zeroed would otherwise
  be invisible to a test that only greps for the happy path.

## Rejected alternatives
- **Keep sharing code pages across processes (COW-lite: map every
  segment read-only+shared, only fault-in a private copy on write)** —
  real demand-paging/COW infrastructure this kernel doesn't have yet
  (no page-fault-driven allocation path exists at all outside boot-time
  eager mapping); doing a half version of it here would be exactly the
  kind of half-finished implementation CLAUDE.md warns against. Deferred
  to when COW is actually built as its own milestone.
- **Support non-page-aligned `PT_LOAD` segments now** — no current
  caller needs it (this repo's own `user.ld` always page-aligns), and
  the correct general handling is materially more complex (shared-page
  permission reconciliation). Rejected for now, explicitly flagged
  above rather than silently assumed unreachable.
- **A general physical-memory direct-map, so segment frames aren't
  constrained to `VMM_IDENTITY_WINDOW_LIMIT`** — already flagged future
  work since ADR 0004; building it now would be scope creep for what
  this milestone actually needed (the window comfortably covers this
  loader's real allocation footprint).

## Verification
- `tests/host/test_elf.c` (new, 10 checks, ASan/UBSan): a valid
  hand-built two-program-header image parses correctly (including that
  a non-`PT_LOAD` header, e.g. `PT_GNU_STACK`, is read back but not
  misidentified as loadable); bad magic, wrong `ELFCLASS`/machine, a
  truncated/overflowing program-header table, an unaligned `p_vaddr`,
  `p_filesz > p_memsz`, and an offset+size that exceeds or overflows
  `image_size` are all independently rejected.
- `make run` (real toolchain) boots and prints, after all Milestones
  1-16 markers unchanged: `[OK] hello from ring 3 via ELF-loaded
  process` then `[OK] elf .data/.bss segment verification passed` —
  **twice**, once per process — then the existing syscall/lifecycle
  self-tests passing with no frame leak (proving `VMM_FLAG_OWNED` on the
  now-private segment frames is correct, not just present).
  `tests/qemu/test_elf_loader_selftest.sh` (new) independently asserts
  the verification message appears exactly twice and the `[FAIL]`
  variant never appears. `test_ring3_syscall_selftest.sh` and
  `test_process_isolation_selftest.sh` needed their expected message
  text updated (stale-marker fixes, not behavior regressions, same
  pattern as every previous milestone transition).
- All sixteen earlier smoke tests and all three pre-existing host test
  suites re-run and pass unmodified (beyond the two marker-text updates
  above).
- Booted 4 times back to back with identical output (2/2 verification
  messages, 0 `[FAIL]` lines, same PML4/syscall-count shape as every
  other run) — correct on the first real boot attempt, no live-diagnosis
  bug this time, unlike Milestones 9 and 16.

## Known limitations (accepted for this milestone only)
No dynamic linking/`PT_INTERP` (static executables only — matches
CLAUDE.md's "no POSIX userland" non-goal territory anyway). No
sub-page-aligned `PT_LOAD` segment support (rejected outright, not
mis-handled — see Decision). No shared/COW text pages (every process
gets a fresh private copy of every segment — see Decision). Still no
filesystem-loaded programs; the ELF image is embedded/statically linked
into the kernel image at build time, same as `user_demo.asm` was. No
`fork`/`exec` — every process is still spawned directly by
`kernel_main` via `task_create_user()`, which now takes no arguments
and always loads the one embedded image; a real multi-program story
needs `exec`-equivalent syscall plumbing, not just a loader, and is
explicitly still on `future.md`'s list.
