# ADR 0004: Virtual memory manager and kernel heap

## Status
Accepted and verified (Milestone 4) — `make run` boots the real ISO and
both the VMM-backed heap and its self-test work; see Verification.

## Context
Milestone 3 gave the kernel a way to know which physical frames are
free. Nothing yet lets the kernel turn a physical frame into usable
memory at an arbitrary virtual address, and `-Wall -Wextra -Werror`
freestanding C with "no malloc/free until kernel heap milestone lands"
(CLAUDE.md) means every kernel subsystem so far has had to use static
arrays for anything that needs storage. This milestone is what removes
that constraint.

## Decision

- **Extend boot.asm's existing PML4 (reused via `CR3`, never reloaded)
  with a brand-new 4KiB-paged region for the heap, rather than replacing
  the kernel image's own 2MiB boot mapping.** `kernel/mm/vmm.c` reads
  `CR3` and walks/creates PDPT→PD→PT levels under it. The kernel image
  itself keeps using boot.asm's original 2MiB huge-page identity +
  higher-half mapping (ADR 0001) completely untouched. Lower blast
  radius for a paging change (CLAUDE.md: "high blast radius, often
  triple-faults silently") — nothing needs the kernel image remapped at
  finer granularity yet, so doing that now would be risk for no benefit.
- **Heap gets its own dedicated 1GiB virtual region:
  `PML4[511]:PDPT[511]` = `0xFFFFFFFFC0000000`**, deliberately not the
  `PDPT[510]` slot the kernel image already occupies
  (`0xFFFFFFFF80000000`) — two independent regions under the same PML4
  entry, built by two independent mechanisms (boot.asm's hardcoded
  tables vs. `vmm_map_page`'s dynamic ones), kept from ever colliding by
  construction rather than by convention.
- **Page-table bootstrap frames must land in boot.asm's identity-mapped
  low 8MiB window, checked explicitly, not assumed.** To write into a
  freshly `pmm_alloc_frame()`'d table (zero it, install PTEs), the
  kernel needs *some* virtual address that reaches that physical frame.
  No general physical-memory direct-map exists (see Rejected
  alternatives), so `get_or_create_table` relies on the identity window
  instead — which holds in practice because `pmm_alloc_frame` always
  returns the lowest-numbered free frame first and `heap_init()` runs
  early (right after `pmm_init`, before almost anything else has
  allocated). This is exactly the kind of paging invariant CLAUDE.md
  says to reason through by hand rather than assume: `vmm.c` checks it
  at runtime (`frame >= VMM_IDENTITY_WINDOW_LIMIT` → `panic()`) instead
  of trusting it silently. Leaf *data* pages (the frames a `kmalloc`
  caller actually gets) have no such constraint — once `vmm_map_page`
  finishes wiring up a PTE, that page is reachable through its own
  virtual address via normal translation, regardless of where its
  physical frame is.
- **`invlpg` after every map/unmap.** CLAUDE.md's TLB-coherence gotcha
  ("TLB is not automatically coherent... or you'll silently keep using
  the stale mapping") — handled once, centrally, inside
  `vmm_map_page`/`vmm_unmap_page`, so no caller can forget it.
- **Heap allocator core logic lives in `libk/heap_alloc.c` as pure,
  hardware-free code; `kernel/mm/heap.c` is a thin `kmalloc`/`kfree`
  wrapper over it, backed by pages `vmm_map_page` prepared.** This
  follows CLAUDE.md's testing philosophy literally ("Host tests:
  allocator logic... run with ASan/UBSan") rather than writing the
  allocator directly against kernel-only plumbing where it couldn't be
  host tested at all. `tests/host/test_heap_alloc.c` exercises alloc/
  free/split/coalesce/OOM/free(NULL) against a plain buffer.
- **First-fit free-list with splitting and coalescing**, not a buddy
  allocator or a bump allocator. Simplest structure that's still
  actually reusable (a bump allocator can't free), and nothing yet
  needs buddy's contiguous-multi-page guarantees. `heap_block_t`'s
  layout is this allocator's own bookkeeping format, not a hardware/spec
  layout (unlike GDT/IDT/page-table entries elsewhere in this kernel) —
  ordinary compiler struct layout is fine, no explicit packing needed
  (same reasoning `trap_frame.h` already documents for its own
  internal-contract struct).
- **Fixed 1MiB initial heap (256 pages), eagerly mapped at
  `heap_init()`, no growth-on-demand yet.** Enough to prove the
  allocator works and back early kernel data structures; nothing
  currently needs more, and growth-on-demand is easy to add later
  without touching the allocator itself (just map more pages and call
  `heap_alloc_init` again over the extended region, or extend it to
  track multiple regions) — not built now because nothing needs it yet.
- **Shared `panic()` factored out (`kernel/panic.c`)**, replacing
  `kernel_main`'s local `static` copy. Three call sites now
  (`kernel_main`, `vmm.c`, `heap.c`) made the duplication real, not
  hypothetical — this isn't "add infrastructure ahead of need," it's
  removing a duplicate that already existed twice and was about to
  become three times.

## Rejected alternatives
- **A general physical-memory direct-map** (map all of physical memory,
  or all 4GiB `pmm` tracks, at a fixed virtual offset so any physical
  frame is trivially addressable) — the "textbook" solution real 64-bit
  kernels use, and would remove the identity-window constraint above
  entirely. Rejected for *this* milestone: it needs the same page-table-
  bootstrap-frame-must-be-writable problem solved first anyway (to build
  the direct map's own tables), and 4KiB-covering-4GiB would be
  wasteful (2MiB or 1GiB pages would fix that, but that's more paging
  machinery than "back a kernel heap" needs). Worth building once
  something genuinely needs to touch arbitrary physical memory by
  address (e.g., mapping a device's MMIO BAR, or a much bigger heap).
- **Reload `CR3` with a freshly built page-table tree** instead of
  extending boot.asm's live one — more "correct" in the sense of not
  depending on boot.asm's tables at all, but a full CR3 switch is a
  bigger, riskier diff (CLAUDE.md: test paging changes incrementally)
  for no benefit this milestone actually needs.
- **Buddy allocator** for the heap — solves external fragmentation for
  multi-page contiguous allocations, which nothing needs yet (every
  `kmalloc` caller so far wants a small, single, non-page-aligned
  block). Reconsider if/when something needs page-aligned or
  multi-page-contiguous kernel allocations.

## Verification
- `tests/host/test_heap_alloc.c`: genuinely host-compiled and run
  (`gcc -fsanitize=address,undefined`), 5 checks — distinct/non-
  overlapping allocations (write-then-verify, not just non-NULL),
  free-then-realloc reuse, 3-way coalescing enabling a larger
  allocation, out-of-memory returns `NULL` without corrupting anything,
  and `heap_alloc_free(heap, NULL)` is a safe no-op. All pass.
- `make run` (real toolchain) boots and prints `[OK] kernel heap
  initialized` then `[OK] heap self-test passed (alloc/write/verify/
  free/reuse)` — the write/verify step specifically proves the two
  `kmalloc`'d blocks don't overlap in real, VMM-mapped memory, not just
  in the host-tested allocator logic against a plain buffer.
- Free-frame count dropped by exactly 2 between the Milestone 3 and
  Milestone 4 boot logs (`0x7eaf`→`0x7ead`), measured at the same point
  in `kernel_main` (right after `pmm_init`, before any self-test
  allocates anything) — consistent with the kernel image itself growing
  by two frames' worth of code (`vmm.c`/`heap.c`/`panic.c`/
  `heap_alloc.c`), not a memory-map-parsing regression. Confirmed by
  reasoning through the number rather than assuming it was fine.
- `tests/qemu/test_heap_selftest.sh` (new) passes; `test_boot_serial.sh`,
  `test_idt_selftest.sh`, and `test_pmm_selftest.sh` (Milestones 1-3)
  all re-run and still pass — confirms extending the live page tables
  didn't regress the kernel image's own boot-time mapping or anything
  built on top of it.
