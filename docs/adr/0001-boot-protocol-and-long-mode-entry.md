# ADR 0001: Boot protocol and 32-to-64-bit long mode entry

## Status
Accepted (Milestone 1).

## Context
The kernel is freestanding x86_64 C11 compiled with `-mcmodel=kernel`
(kernel code/data assumed to live in the top 2GiB of the virtual address
space) and `-mno-sse`/`-mno-sse2`/etc. None of that is true at the moment
a legacy PC boot chain hands off control: the CPU starts in real mode,
and even a Multiboot-compliant loader only gets us to 32-bit protected
mode with paging disabled. Something has to bridge from there to 64-bit
long mode, with the kernel's higher-half virtual mapping live, before a
single line of `kernel.c` can run.

## Decision
- **Boot protocol: GRUB2 / Multiboot2.** (CLAUDE.md lists Limine as the
  stated preference but allows GRUB2/Multiboot2 as an alternative,
  flagged for confirmation — confirmed by the user for Milestone 1.)
  Header fields, magic values, and the end-tag layout were verified
  against the canonical GRUB `multiboot2.h`
  (https://raw.githubusercontent.com/rhboot/grub2/master/include/multiboot2.h)
  rather than recalled from memory:
  - `MULTIBOOT2_HEADER_MAGIC = 0xe85250d6`
  - `MULTIBOOT2_BOOTLOADER_MAGIC = 0x36d76289` (found in EAX at entry)
  - header = `{ magic, architecture, header_length, checksum }` (all `u32`)
  - `MULTIBOOT2_ARCHITECTURE_I386 = 0`
  - end tag = `{ type=0, flags=0, size=8 }`, 8-byte aligned
  Header placement within the required low-offset window is verified
  mechanically, not assumed: `make check-mb2` runs
  `grub-file --is-x86-multiboot2` against the linked ELF.

- **32→64-bit transition: hand-rolled in `kernel/arch/x86_64/boot.asm`
  (NASM),** following the Intel SDM Vol. 3A §9.8.5 "Initializing IA-32e
  Mode" sequence: enable `CR4.PAE` → load `CR3` → set `IA32_EFER.LME` →
  enable `CR0.PG` → far jump (`jmp CODE_SEL:...`) to reload `CS` with an
  `L=1` GDT code descriptor. GDT entries are written byte-by-byte in the
  source with the access/flags bits spelled out in binary and commented,
  rather than as opaque magic constants, per CLAUDE.md's bit-packed-layout
  rule.

- **Paging for boot: 2MiB pages, no 4KiB-page (PT) level, 8MiB mapped
  twice.** A single shared page directory (`pd_shared`) is pointed to by
  both `pdpt_low[0]` (identity: VA 0x0 = PA 0x0) and `pdpt_high[510]`
  (higher half: VA `0xFFFFFFFF80000000` = PA 0x0), so the low 8MiB of
  physical memory is simultaneously visible at its identity address and
  at `PA + 0xFFFFFFFF80000000`. The PML4/PDPT indices for the higher-half
  base were computed, not eyeballed: `(0xFFFFFFFF80000000 >> 39) & 0x1FF
  = 511`, `(>> 30) & 0x1FF = 510`, `(>> 21) & 0x1FF = 0` (checked with
  `python3` during implementation). This is a placeholder mapping —
  Milestone 4 (paging/VMM + kernel heap) replaces it with real 4KiB
  mappings, a frame allocator, and per-mapping permissions; anything
  compiled/linked above 8MiB physical will silently page-fault against
  this scheme, which is an accepted, temporary limitation of Milestone 1
  only.

- **`boot/linker.ld` higher-half trick:** the location counter is bumped
  by `KERNEL_VMA_OFFSET = 0xFFFFFFFF80000000` right after the low boot
  trampoline, and every later section uses `AT(ADDR(...) -
  KERNEL_VMA_OFFSET)` to keep its load address contiguous in physical
  memory. This means "virtual address of any kernel symbol" and
  "physical address" always differ by exactly `KERNEL_VMA_OFFSET`, which
  is also exactly the offset baked into the boot page tables above — the
  two are deliberately the same constant so nothing has to special-case
  the kernel's own 1MiB load offset.

- **One boot stack, no separate "kernel stack" yet.** `boot_stack_top`
  (32KiB, `.boot.bss`) is used all the way from `_start` through
  `kernel_main`, since it stays valid at its identity VA under the shared
  page-directory scheme above. A distinct, per-context stack concept
  arrives with interrupts (IST stacks, Milestone 2) and the scheduler
  (per-thread stacks, Milestone 6).

## Rejected alternatives
- **Limine boot protocol** — CLAUDE.md's stated preference, and simpler
  (Limine hands off already in long mode with paging set up, no manual
  GDT/page-table bring-up needed). Not used for Milestone 1 because the
  user explicitly chose GRUB2/Multiboot2 when the boot-protocol decision
  was flagged for confirmation. Revisiting this is a boot-protocol change
  and needs its own confirmation + ADR update, not a silent switch.
- **4KiB pages for the boot mapping** — more correct long-term (matches
  where the real VMM is headed) but adds a page-table (PT) level and a
  loop/table-fill step for no benefit at this milestone, where the only
  requirement is "map enough low memory to run a few KB of kernel code."
  Deferred to Milestone 4.
- **GRUB2 Multiboot (v1) instead of Multiboot2** — simpler header, but
  CLAUDE.md's gotcha list already anticipates Multiboot2, and v2's
  tag-based info structure is what later milestones (memory map parsing
  for the frame allocator) will actually want.

## Verification
- `make check-mb2` (`grub-file --is-x86-multiboot2`) confirms the linked
  `kernel.elf` carries a header GRUB2 recognizes, mechanically — not by
  inspecting the linker script by eye.
- `tests/qemu/test_boot_serial.sh` boots the produced ISO headless under
  QEMU TCG (`-display none -serial file:...`) with a timeout, and asserts
  the literal marker `[OK] hello kernel` appears in the captured serial
  output — proving control actually reached `kernel_main` in 64-bit mode
  with the UART working, not just that the image linked and booted to
  *some* state.
- Any future change to `boot.asm`, `boot/linker.ld`, or the page-table
  setup should be re-verified the same way, plus `-d int,cpu_reset` if
  a triple fault is suspected, per CLAUDE.md's paging/boot testing rule.
