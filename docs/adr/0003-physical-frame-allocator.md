# ADR 0003: Physical frame allocator

## Status
Accepted and verified (Milestone 3) — `make run` boots the real ISO and
the self-test passes; see Verification.

## Context
Milestone 4 (paging/VMM + kernel heap) needs a source of physical page
frames to build new page tables and back the heap with. Before that can
exist, something has to know which physical addresses are actually
usable RAM (vs. reserved/ACPI/MMIO/already-occupied-by-the-kernel) and
hand out 4KiB frames one at a time without double-allocating.
`kernel_main` was already receiving `mbi_addr` (the Multiboot2 info
pointer) since Milestone 1 but ignoring it — this is the milestone that
actually reads it.

## Decision

- **Parse the Multiboot2 memory map tag (type 6) to find usable RAM.**
  Tag/entry struct layout verified against the same primary source ADR
  0001 used for the boot header
  (https://raw.githubusercontent.com/rhboot/grub2/master/include/multiboot2.h):
  `multiboot_tag {type,size}`, `multiboot_tag_mmap
  {type,size,entry_size,entry_version}`, `multiboot_mmap_entry
  {addr,len,type,zero}`, `MULTIBOOT_MEMORY_AVAILABLE=1`. The 8-byte
  fixed info-structure header (`total_size`,`reserved`) that precedes
  the tag list isn't in that C header (GRUB builds it inline,
  bootloader-side) but is standard, stable Multiboot2 spec structure.
- **Bitmap allocator, one bit per 4KiB frame, fixed-size static array
  covering 4GiB (128KiB bitmap) rather than sized dynamically from the
  detected memory map.** There is no heap yet to size it into — that's
  literally the next milestone — so dynamic sizing would need its own
  bootstrap allocator, which is more machinery than a bitmap needs.
  4GiB is comfortably more than this hobby kernel or its QEMU dev target
  need for now (QEMU's default RAM is far smaller; the real machine
  running this is a dev box, not a server). Frames above 4GiB are simply
  never tracked or allocatable — an accepted, documented limit, not an
  oversight. Revisit if/when a target actually has more RAM than that.
- **Default-deny memory map parsing: every frame starts marked used;
  the map only ever frees frames, never the reverse.** This is
  bootloader-supplied external input (CLAUDE.md: validate sizes/offsets
  in any parser), so a map this code fails to fully parse just means
  less usable memory gets discovered, never a frame the kernel
  mistakenly believes is free. Reserved ranges (kernel image, multiboot
  info structure, physical address 0) are carved out with a *second*
  pass, after the memory map's "available" ranges are applied — GRUB's
  map has no idea the kernel is sitting inside one of those ranges, so
  reserving it first would just get overwritten as "free" by the map.
- **Physical address 0 is permanently reserved and doubles as the
  allocation-failure sentinel.** `pmm_alloc_frame()` returns `0` both
  when the allocator is exhausted and — trivially, since frame 0 is
  never handed out — whenever nothing went wrong but the caller checks
  for zero, so a real allocation is never confused with "no frames
  left" or with a null pointer once a physical frame becomes a virtual
  mapping in Milestone 4.
- **Fixed the underlying reason a bitmap-based allocator is safe to
  build at all: `.bss` is now explicitly zeroed before `kernel_main`
  runs** (`boot/linker.ld`'s new `__bss_start`/`__bss_end` symbols,
  zeroed by `boot.asm`'s `higher_half_entry` via `rep stosb`, before the
  `.multiboot_magic`/`.multiboot_info_ptr` loads and the `call
  kernel_main`). Nothing previously guaranteed this: GRUB only loads a
  file's real (PROGBITS) content, and `.bss` is `NOLOAD` — the backing
  physical RAM could hold arbitrary leftover firmware/prior-boot data.
  Milestone 1/2's statics (`idt[256]`, `gdt[3]`) happened to be safe
  anyway because their own `init()` functions always fully overwrite
  every element before any read, so there was no live bug — but
  `pmm.c`'s bitmap is deliberately allowed to rely on C's normal
  zero-init guarantee (`static uint8_t frame_bitmap[...]`, no explicit
  clear-loop needed beyond the `0xff` "default deny" pass, which itself
  reads as "start from a known state" rather than "work around
  undefined memory"), and that's only safe kernel-wide now that this is
  fixed for real instead of by convention.

## Rejected alternatives
- **Size the bitmap dynamically from the detected memory map, placed
  in the highest usable physical region found** — the "textbook
  correct" approach, but needs a place to put it before any allocator
  exists to give it one (a classic bootstrap problem), and buys nothing
  this kernel needs yet (see 4GiB rationale above). Revisit only if a
  real target's RAM ever gets close to that limit.
- **Free-list / stack-of-frames instead of a bitmap** — O(1) alloc/free
  instead of the bitmap's O(n) linear scan, but needs the frames
  themselves to be *addressable* to store list-node pointers in them,
  which isn't true yet for arbitrary physical memory (only the low 8MiB
  boot.asm identity-maps is addressable at all pre-Milestone-4). A
  bitmap only needs to be addressable itself (it's kernel `.bss`), not
  the frames it describes. Worth reconsidering once Milestone 4's VMM
  makes all tracked physical memory addressable.
- **Buddy allocator** — solves external fragmentation for
  multi-frame/contiguous allocations, which nothing in this kernel needs
  yet (single-frame allocations only, until the heap or DMA buffers need
  otherwise). Overkill for what's actually used right now.

## Verification
- Tag/entry struct fields verified against GRUB's actual
  `multiboot2.h`, not recalled from memory (see Decision).
- `make run` (real `x86_64-elf-gcc`/`binutils` build) boots and prints:
  `[OK] pmm initialized, free frames: 0x7eaf / total: 0x100000`, then
  `[OK] pmm self-test passed (alloc/free/reuse)`. `0x100000` frames
  total = exactly 4GiB / 4KiB, confirming `PMM_MAX_FRAMES`'s arithmetic.
  `0x7eaf` = 32431 frames ≈ 126.7MiB free, consistent with QEMU's
  default RAM allocation minus the reserved kernel/multiboot/low-memory
  ranges — a sanity-checkable number, not just "some nonzero value."
- `tests/qemu/test_pmm_selftest.sh` (new): boots and asserts both the
  init marker and the self-test-passed marker, plus a numeric sanity
  check that the reported free-frame count is greater than zero
  (extracted from the actual serial output, not hardcoded, since the
  exact figure is QEMU-RAM-config-dependent).
- `tests/qemu/test_boot_serial.sh` and `tests/qemu/test_idt_selftest.sh`
  (Milestones 1/2) re-run and still pass — confirms the `.bss`-zeroing
  change didn't regress GDT/IDT init or the exception self-test.
- `tests/host/test_fmt.c` re-run and still passes (unaffected by this
  milestone, but re-checked since `libk/fmt.c` is a dependency of the
  new init-summary print).
