# ADR 0021: Copy-on-write fork

## Status
Accepted and verified — `make run` boots the real ISO, a new self-test
(`vmm_get_cow_fault_count()`) confirms pages are actually shared
lazily (a real `#PF` was resolved, not just "correct by luck because
nothing was ever written"), and `kernel/user/fork_demo.asm`'s own
isolation check confirms the sharing is genuinely private, not
aliased. Verified via `-d int,cpu_reset` — exactly 3 `#PF` events this
boot (matching the 3 deliberate COW faults `fork_demo.asm`'s write
sequence produces), zero double-fault/reset events. Correct on the
first real boot attempt; booted 5 times back to back with identical
shape. All twenty pre-existing smoke tests and all four host test
suites re-verified passing with no regression, including the
frame-leak count in `test_process_lifecycle_selftest.sh` (refcounted
frames return to the EXACT same baseline an eager copy would have,
once every reference is actually dropped).

## Context
ADR 0018 shipped `sys_fork()` as a full eager deep copy, with a named
reason: no copy-on-write mechanism existed yet. `future.md` flagged
this as the natural next memory-maturity item, specifically once two
things existed to build it on: a concrete motivating case (ADR 0018's
own eager copy, wasteful for any page a child never actually
modifies) and Milestone 19's general physical direct-map
(`vmm_phys_to_virt()`), which makes a lazy page-fault-driven copy's
own bookkeeping (reading/writing an arbitrary physical frame's
content without needing it mapped anywhere else first) simple to
build.

## Decision

- **Refcounted frames, not a parallel "shared page" bookkeeping
  structure.** `kernel/mm/pmm.c` gained a `frame_refcount[]` array
  (same flat-array-indexed-by-frame-number convention `frame_bitmap[]`
  already uses, one `uint16_t` per frame — 2MiB total, an accepted
  fixed cost matching the codebase's existing "4GiB-worth of fixed
  bookkeeping" tradeoff, ADR 0003). `pmm_alloc_frame()` sets a fresh
  frame's refcount to 1 (its one implicit owner); the new
  `pmm_frame_addref()` is the only way to push it higher (called
  exactly once per NEW page-table reference a frame gains — i.e. once
  per `vmm_fork_cow_page()` call, when the child's mapping is
  created); `pmm_free_frame()` now decrements first and only actually
  reclaims the frame (clears the bitmap, back into the free pool) once
  the count reaches 0. Every EXISTING call site (every non-COW
  `pmm_alloc_frame()`/`pmm_free_frame()` pair in this codebase) is
  unaffected: refcount goes 1 -> 0 exactly as before, identical
  behavior, since none of them ever call `pmm_frame_addref()`.
- **Downgrade the PARENT's own existing mapping to COW in place at
  fork time, not just the child's new one.** `vmm_fork_cow_page()`
  (`kernel/mm/vmm.c`), called once per writable page `task_fork()`'s
  `vmm_for_each_user_page()` visitor finds, uses a new `find_pte()`
  walker to reach the PARENT's own live PTE directly (safe because
  `task_fork()` only ever runs under the forking process's OWN CR3,
  ADR 0007/0018 — the same fact ADR 0018 already relied on to read the
  parent's pages via ordinary virtual addresses) and clears
  `VMM_FLAG_WRITABLE`/sets the new `VMM_FLAG_COW` bit on it, then maps
  the SAME physical frame into the child with the identical read-only
  + COW treatment. A page that was never writable in the first place
  (an ELF image's `.text`/`.rodata`) skips all of this — shared
  outright, permanently, no COW bit needed, since neither side can
  ever write it anyway.
- **`VMM_FLAG_COW` is a new AVL bit (bit 10)**, distinct from the
  existing `VMM_FLAG_OWNED` (bit 9) so a leaf can independently be
  "this address space's own reference, to be freed/refcount-dropped on
  exit" (OWNED, unchanged meaning) and "currently shared, not yet
  privately copied" (COW, cleared the moment either sibling actually
  writes it) — a COW page stays OWNED throughout; `vmm_destroy_address_
  space()`'s existing OWNED-gated `pmm_free_frame()` call needed ZERO
  changes to correctly drop a still-shared page's reference (refcount-
  aware `pmm_free_frame()` handles the rest).
- **The actual fault handler lives in `vmm_handle_cow_fault()`, called
  from `kernel/arch/x86_64/exceptions.c`'s `isr_handler` (`#PF`,
  vector 14) BEFORE any of the existing diagnostic-dump/panic code
  runs** — checked first: error code bits P=1 (protection violation on
  an ALREADY-present page, Intel SDM Vol. 3A Sec. 4.7 Table 4-12) and
  W=1 (a write, not a read); only if both hold does it even call
  `vmm_handle_cow_fault()`, and only if THAT returns true does
  `isr_handler` return the frame unmodified (resume right at the
  faulting instruction, which now succeeds) instead of falling through
  to the fatal panic path every other exception still takes. A COW
  fault is expected, frequent, successful control flow — deliberately
  silent (no diagnostic printing at all), the same "the common/
  successful case doesn't get logged" reasoning already applied
  elsewhere in this kernel (e.g. `sys_nop`).
- **The standard real-COW optimization: skip the copy if this is
  already the last reference.** `vmm_handle_cow_fault()` checks
  `pmm_frame_refcount()` on the shared frame first — if it's already
  down to 1 (every sibling that shared it has already copied out, or
  there never was one to begin with), it just flips `WRITABLE` on and
  `COW` off IN PLACE, no allocation, no copy. Only when the count is
  still > 1 does it `pmm_alloc_frame()` a new frame, copy the shared
  frame's content via `vmm_phys_to_virt()` on both sides (Milestone
  19), remap this address space's own leaf to the new frame, and
  `pmm_free_frame()` the old one (dropping just this address space's
  reference).
- **Runs entirely with interrupts masked — confirmed from `idt.c`, not
  assumed.** Every exception vector (0-31, including `#PF`) is
  installed as an INTERRUPT gate (`present_dpl0_interrupt_gate`,
  `idt_init()`), which auto-clears `IF` on entry per the SDM, and
  nothing in `isr_handler`'s COW path (or anywhere else in
  `exceptions.c`) ever `sti`s. This means `vmm_handle_cow_fault()` can
  never be preempted mid-resolution, so it can never race another
  task's own fault or fork touching the same shared frame's refcount —
  no new synchronization primitive needed, same "interrupts-disabled
  section IS the mutual-exclusion primitive" reasoning CLAUDE.md safety
  rule 1 already establishes for every other piece of shared state in
  this single-CPU kernel.
- **Verification proves BOTH halves independently: that sharing is
  genuinely lazy, AND that it's genuinely isolated.** These are
  different failure modes and neither implies the other (a broken
  implementation that eagerly copies at fork time would still pass an
  isolation-only check; one that aliases instead of copying would
  still pass a fault-count-only check). `vmm_get_cow_fault_count()`
  (new counter, incremented once per resolved fault) proves laziness —
  `kernel_main` panics if it's ever 0 after the fork demo runs, the
  same "prove the new behavior was actually exercised, not just
  correct by luck" pattern Milestone 20's `syscall_get_wait_block_
  count()` already established. `kernel/user/fork_demo.asm`'s own
  isolation check proves correctness: a `.data` variable (`shared_var`,
  a real writable `PT_LOAD` segment, COW-eligible) starts at a known
  baseline identical in parent and child right after fork; the parent
  writes one sentinel, the child writes a DIFFERENT one and exits, and
  the parent's readback — guaranteed to happen strictly AFTER the
  child's own write and full exit, via Milestone 20's blocking
  `sys_wait` — must observe its OWN sentinel, not the child's. Reusing
  the just-shipped blocking `sys_wait` as this test's own
  synchronization primitive needed no new IPC/sync machinery.

## Rejected alternatives
- **A separate sparse "shared pages" table instead of a flat
  per-frame refcount array** — rejected: more moving parts for no
  real benefit at this kernel's scale (4GiB / 4KiB = ~1M frames, 2MiB
  of `uint16_t` refcounts is a fixed, acceptable cost matching the
  bitmap's own sizing convention), and a flat array indexed by frame
  number is exactly the pattern `frame_bitmap[]` already established
  as this codebase's answer to "track one small fact per frame."
- **Track VMAs (a real per-process memory map) as part of this
  milestone** — rejected: COW doesn't need them (the existing
  `vmm_for_each_user_page()`/PTE-flag-driven approach is sufficient),
  and `future.md` already flags VMAs as a SEPARATE future item; folding
  it in here would violate CLAUDE.md's "one subsystem per change"
  discipline for no benefit this milestone actually needs.
- **`uint8_t` refcounts (1MiB instead of 2MiB)** — rejected: cheap
  extra headroom (65535 vs. 255 max concurrent sharers of one frame)
  for a hobby kernel with no realistic path to needing it, not worth
  the tighter, easier-to-silently-wrap limit.
- **Have `vmm_handle_cow_fault()` re-validate the fault is even in a
  process-private region before touching anything** — rejected as
  redundant: `find_pte()` already returns `NULL` (treated as "not a
  COW fault") for any address with a not-present intermediate table
  entry, and every process-private region's own leaf either has
  `VMM_FLAG_COW` set (handled) or doesn't (falls through to the
  existing fatal path) — no case exists where a SEPARATE region check
  would catch something the existing walk doesn't already handle
  correctly.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-20
  marker unchanged, plus (new) `[OK] COW isolation verified: parent's
  write survived the child's own write` and `[OK] copy-on-write
  self-test passed, 0x3 page(s) copied lazily on first write, not
  eagerly at fork time` — `kernel_main` PANICS if the fault count is
  ever 0. The fault count (3) matches hand-verified reasoning: the
  parent's own write to its `child_pid` `.bss` slot (1), the parent's
  write to `shared_var` (1, takes the COPY branch since the child
  hasn't written yet — refcount still 2), and the child's own later
  write to `shared_var` (1, takes the IN-PLACE-TAKEOVER branch since
  the parent already dropped its reference — refcount down to 1 by
  then).
- `-d int,cpu_reset` trace across a full boot: exactly 3 `#PF`
  (`v=0e`) events — matching the reasoning above exactly, not just
  "some nonzero number" — zero double-fault (`v=08`), zero reset
  events, only expected IRQ0/IRQ1/IRQ12 traffic and the pre-existing
  deliberate `#BP`. This milestone is the first to make `#PF` an
  EXPECTED vector in this trace rather than a zero-tolerance one — the
  exact count is what's checked now, not just its absence.
- `tests/qemu/test_cow_fork_selftest.sh` (new): checks both new
  markers, that the reported fault count is >= 3, and that the
  frame-leak self-test (`test_process_lifecycle_selftest.sh`'s own
  assertion, re-checked here too) still passes — proving refcounted
  frames return to the exact same baseline an eager copy would have.
  All twenty earlier smoke tests and all four host test suites
  re-verified passing, with one header-comment update
  (`test_fork_wait_selftest.sh`: "deep copy" -> current behavior; its
  actual assertions needed no changes). Booted 5 times back to back —
  correct every time, no flakiness.

## Known limitations (accepted for this milestone only)
No demand paging / lazy allocation beyond fork's own COW sharing —
every OTHER page (a fresh stack, a newly `mmap`-equivalent region, if
one existed) is still eagerly allocated exactly as before; COW only
applies to pages `task_fork()` shares from an existing parent. No VMA
tracking (see Rejected alternatives) — a future milestone's own
concern. `frame_refcount[]` is `uint16_t`: a single frame shared by
more than 65535 concurrent address spaces would silently wrap,
accepted as unreachable for this kernel's realistic scope (no
shell-driven exec/fork-loop workload exists yet that could approach
it). `vmm_handle_cow_fault()` trusts that any `VMM_FLAG_COW`-tagged
present PTE it finds is exactly what `vmm_fork_cow_page()` produced
(consistent flags, a frame `pmm_alloc_frame()` actually issued) — same
internal-caller trust boundary every other raw-PTE-bit consumer in
this codebase already relies on.
