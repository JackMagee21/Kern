# ADR 0019: General physical-memory direct-map

## Status
Accepted and verified — `make run` boots the real ISO, the direct-map's
own self-test (write through `vmm_phys_to_virt()`, read back via the
completely independent low identity mapping) passes, and both
downstream consumers switched over to it this milestone (the ELF
loader, fork's page copy) still work correctly with no regression.
Verified via `-d int,cpu_reset` per CLAUDE.md's explicit prescription
for paging changes — zero unexpected `#PF`/double-fault/reset events in
the trace, only the deliberate `#BP` self-test. Correct on the first
real boot attempt. All eighteen earlier smoke tests and all four host
test suites re-verified passing.

## Context
ADR 0004 accepted, as a known limitation, that only the low 8MiB
`boot.asm` identity-maps (`VMM_IDENTITY_WINDOW_LIMIT`) is directly
writable via a raw physical-address-as-pointer cast — flagged to
revisit "only when something actually needs more." By Milestone 18,
three separate subsystems depended on that same constraint
independently: `vmm.c`'s own page-table bootstrap frames (irreducible,
see Decision), Milestone 17's ELF loader (a segment's destination
frame), and Milestone 18's `task_fork()` (a copied page's destination
frame). Two real, working-but-constrained subsystems hitting the same
documented limitation is exactly the "something actually needs more"
trigger ADR 0004 named — and, per `future.md`'s "reasonable next steps"
this session already flagged, a general physical-memory direct-map was
the natural next memory-maturity item.

## Decision

- **A direct-map, not exec-shaped fixes to the two call sites.** Rather
  than teaching `elf_load()`/`task_fork()` some new per-caller
  workaround, `vmm_direct_map_init()`/`vmm_phys_to_virt()`
  (`kernel/mm/vmm.c`) solve the general problem once: map ALL 4GiB
  `pmm.h`'s bitmap tracks at a fixed virtual base, so ANY
  `pmm_alloc_frame()` result is directly writable via
  `vmm_phys_to_virt(frame)`, unconditionally.
- **2MiB pages, not 1GiB or 4KiB.** 1GiB pages would need a new CPUID
  feature check (`Page1GB`, not guaranteed present, unlike NX which
  Milestone 11 already verified is) for a one-time, non-performance-
  critical bootstrap mapping — not worth the new dependency. 4KiB pages
  would need over a million `vmm_map_page`-style leaf entries for 4GiB;
  2MiB pages need only 2048 PD entries across 4 PDPT tables, and this is
  the SAME encoding (`PTE_PS`, Intel SDM Vol. 3A Sec. 4.5) `boot.asm`'s
  own identity map already proved correct — not a fresh guess.
- **Placed at PDPT[505..508] (4 x 1GiB = 4GiB), under the SAME shared
  PML4[511] entry every other kernel-half region (heap `PDPT[511]`,
  kernel image `PDPT[510]`, kernel stacks `PDPT[509]`, ADR 0012) already
  lives under — verified free via `python3`, not assumed, matching this
  codebase's established discipline for every PML4/PDPT index decision
  (ADR 0012).** This is deliberate, not incidental: `PML4[511]` is
  copied BY REFERENCE into every new process's address space
  (`vmm_create_address_space()`, ADR 0009), so putting the direct-map
  there means it's automatically, permanently visible from kernel code
  running under ANY process's `CR3` — exactly the access pattern
  `elf_load()`/`task_fork()` both need, since neither can assume the
  kernel's own original bootstrap `CR3` is the one currently active
  (`task_fork()` explicitly runs mid-syscall, under the FORKING
  process's own `CR3`).
- **Ordering requirement: must run before the first
  `vmm_create_address_space()` call.** Since the PML4 entry is shared
  by REFERENCE (copying the PML4[511] *pointer*, not a snapshot of its
  contents at process-creation time), this only matters at the moment a
  NEW address space's `PML4[511]` entry is first populated — but that
  populate-from-current step only happens once, when
  `vmm_create_address_space()` reads `get_pml4()[511]`. If the direct
  map's PDPT entries didn't exist YET at that moment, the copied
  pointer would be to a PDPT missing them, and no LATER
  `vmm_direct_map_init()` call could retroactively fix an
  already-created process's table. `kernel_main` enforces this by
  calling it immediately after `pmm_init()`, well before `heap_init()`
  or any process/fork ever runs — the same ordering discipline ADR
  0009's PML4[0]/[511] sharing design already established.
- **Does NOT eliminate `VMM_IDENTITY_WINDOW_LIMIT` — narrows what it
  applies to.** `vmm.c`'s own page-table bootstrap frames
  (`get_or_create_table`, `vmm_create_address_space`, and
  `vmm_direct_map_init` itself) still need it: they allocate the very
  page tables the direct-map depends on, so they can't be bootstrapped
  via a direct-map that doesn't exist yet at the point they're needed —
  an irreducible chicken-and-egg constraint, not an oversight. DATA
  frames (an ELF segment's content, a forked page's content) no longer
  need it — `elf_loader.c` and `task_fork()`'s `fork_copy_page()` were
  both switched to `vmm_phys_to_virt()` this milestone, and their
  `>= VMM_IDENTITY_WINDOW_LIMIT` panic checks removed (no longer a
  reachable failure mode for them).
- **No `invlpg` needed for the direct-map's own setup.** Every PD entry
  it writes is a transition from not-present to present, never a change
  to an already-cached translation — Intel SDM Vol. 3A Sec. 4.10
  confirms the TLB never caches a not-present translation as valid, so
  there's nothing stale to invalidate. Same reasoning
  `vmm_map_page_in()`'s own doc comment already relies on for a fresh
  mapping; this isn't a new argument invented for this milestone.
- **Self-test cross-checks two INDEPENDENT translation paths, not just
  "reads back what it wrote."** Writes a pattern to a fresh frame
  through `vmm_phys_to_virt()`, then reads it back through the raw low
  identity mapping (valid specifically because so few frames have been
  allocated by this point in boot that the test frame is guaranteed
  still within the 8MiB identity window — the same "pmm hands out the
  lowest-numbered free frame first, and this runs early" reasoning
  `VMM_IDENTITY_WINDOW_LIMIT`'s own doc comment already relies on).
  Two different address-translation mechanisms agreeing on the same
  physical byte is real evidence the direct-map's base-address
  arithmetic is correct, not just internally self-consistent.

## Rejected alternatives
- **Fix `elf_loader.c`/`task_fork()` independently with narrower,
  local workarounds** (e.g., restrict their frame allocations to the
  existing 8MiB window, same as page tables) — rejected: this is
  exactly the "something actually needs more" trigger ADR 0004
  predicted, and a third subsystem hitting the identical limitation
  would eventually force the same general fix anyway; solving it once,
  generally, is less total work and removes a recurring source of
  panics as physical memory usage grows.
- **1GiB pages** — would need a new CPUID feature check with no
  fallback story for a CPU that doesn't support it; rejected for a
  one-time bootstrap mapping where the extra complexity buys nothing
  performance-relevant. See Decision.
- **Map the direct-map into its own dedicated, NOT-PML4[511]-shared
  region, with per-process re-establishment** — rejected: would need
  every `vmm_create_address_space()` caller to redundantly rebuild the
  same 2048 PD entries per process, wasteful, and reintroduces exactly
  the "which CR3 is active" fragility ADR 0009 already worked through
  once for the identity map/kernel-half sharing design.

## Verification
- `make run` (real toolchain) boots and prints, after the existing
  `[OK] pmm self-test passed` marker (Milestone 3, unchanged): `[OK]
  physical memory direct-map initialized` then `[OK] direct-map
  self-test passed (write via vmm_phys_to_virt visible via the low
  identity mapping)`, strictly BEFORE `[OK] kernel heap initialized` —
  then every Milestone 1-18 marker through the shell prompt unchanged,
  including both ELF-loaded processes' `.data`/`.bss` verification and
  the fork/wait demo's exit-code verification (both now exercising
  `vmm_phys_to_virt()` under the hood, proving the refactor didn't
  regress either).
- `-d int,cpu_reset` trace across a full boot: zero `#PF` (`v=0e`),
  zero double-fault (`v=08`), zero reset events — only the deliberate
  `#BP` self-test, confirmed by direct grep of the trace file, not
  eyeballed.
- `tests/qemu/test_direct_map_selftest.sh` (new): checks both new
  markers, their ordering relative to `kernel heap initialized`, and
  that the downstream ELF-loader/fork-demo self-tests (which now
  exercise the new code path) still pass. All eighteen earlier smoke
  tests and all three pre-existing host test suites re-run and pass
  unmodified — this milestone needed zero marker-text updates to
  existing tests, the first of Milestones 9-19 that didn't.
- Booted 4 times back to back with identical shape each time — correct
  on the first real attempt, no flakiness.

## Known limitations (accepted for this milestone only)
Fixed 4GiB coverage, matching `pmm.h`'s own bitmap tracking limit
exactly (ADR 0003's known limitation: frames above 4GiB physical are
never tracked/allocatable at all, so the direct-map covering exactly
what `pmm` can ever hand out is not an independent new limitation).
`vmm_phys_to_virt()` does not validate its input is an actual
`pmm_alloc_frame()`-issued frame — same trust boundary as every other
raw-physical-address consumer in this codebase; a caller passing a
bogus value is a caller bug, not something this function detects.
`vmm.c`'s own page-table bootstrap frames still need
`VMM_IDENTITY_WINDOW_LIMIT` — irreducible, not an oversight (see
Decision).
