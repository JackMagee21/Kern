# Continuing this project

This file is a handoff briefing for whoever (human or AI) picks this
project up next. It is not itself a design document — `docs/roadmap.md`
and `docs/adr/*` are the authoritative record of what was built and
why. This file exists to orient you quickly: what state the kernel is
in, what's explicitly waiting on a decision, and what a reasonable next
step looks like.

## What this is

A hobby OS kernel, freestanding C11, x86_64, built from scratch
following `/CLAUDE.md`'s discipline (boot → memory → scheduler →
userspace → hardening → drivers, each step proven by an actual QEMU
boot, not just "it compiled"). Started as a minimal serial-only kernel
and grew, milestone by milestone, into a preemptive multi-process
kernel with per-process address spaces, NX/guard-page hardening, and a
handful of real hardware drivers.

## State as of Milestone 19 (2026-08-24)

Everything below is DONE, verified via actual QEMU boots (not just
compiled), and committed. Read `docs/roadmap.md` for the full list with
verification details; read the corresponding `docs/adr/NNNN-*.md` for
the design reasoning and any real bugs found along the way.

1. Boot → serial hello (Multiboot2, long mode, higher-half)
2. GDT + IDT + exception handling (full register/fault dump on panic)
3. Physical frame allocator
4. Paging/VMM + kernel heap
5. PIC/PIT + IRQ handling
6. Preemptive round-robin scheduler
7. Ring 3, syscalls (`sys_nop`/`sys_write`), one shared-address-space
   demo process
8. VGA text console, PS/2 keyboard, interactive shell
   (`help`/`echo`/`uptime`/`clear`)
9. **Per-process address spaces** — every ring-3 process gets its own
   private page tables (shared kernel-half + identity-map entries only)
10. **Process lifecycle** — `sys_exit`, a reaper task that actually
    frees a dead process's address space/stacks, verified via an exact
    before/after free-frame count (no leak)
11. **NX enforcement** — kernel heap and process stacks are genuinely
    non-executable (W^X); verified by reading back real page-table bits
12. **Kernel stack guard pages** — every kernel-mode stack (kernel
    threads and ring-3 processes' kernel stacks) has a deliberately
    unmapped guard page below it
13. **PCI enumeration** — brute-force bus scan via legacy Configuration
    Mechanism #1, no ACPI dependency
14. **CMOS RTC driver** + a `date` shell command
15. **Legacy (non-ACPI) reboot** — 8042 controller reset + triple-fault
    fallback, `reboot` shell command
16. **PS/2 mouse driver** — IRQ12, standard 3-byte packet decode, a
    `mouse` shell command (no cursor/graphics to draw yet — see below)
17. **ELF64 loader for ring-3 processes** — `libk/elf.h/.c` (host-tested
    parser) + `kernel/mm/elf_loader.c` (kernel-only mapper): every
    ring-3 process now parses and maps a REAL compiled ELF64 executable
    (`kernel/user/hello.asm` + `user.ld`, embedded via `incbin`) with
    real per-segment W^X derived from the file's own program headers,
    replacing Milestone 7-16's single hand-mapped raw code blob
    (`user_demo.asm`, retired). Each process gets a fresh private copy
    of every segment (no shared/COW text pages — a deliberate,
    documented tradeoff, see ADR 0017).
18. **`sys_fork`, non-blocking `sys_wait`, exit codes** — `task_fork()`
    (`kernel/sched/task.c`) deep-copies a process's entire address
    space (`vmm_for_each_user_page()`, new) into a genuinely
    independent child, resuming it via a synthetic trap frame built
    from the parent's in-flight syscall state. `sys_wait` is
    deliberately non-blocking (this kernel's syscalls are still
    non-preemptible with interrupts masked throughout, ADR 0007 — a
    real blocking wait needs a scheduler primitive that doesn't exist
    yet) — the caller polls, proven end to end by
    `kernel/user/fork_demo.asm`. See ADR 0018.
19. **General physical-memory direct-map** — `vmm_direct_map_init()`/
    `vmm_phys_to_virt()` (`kernel/mm/vmm.c`) map the full 4GiB `pmm.h`
    tracks at a fixed virtual base (2MiB pages, under the shared
    `PML4[511]` kernel-half entry). Closes the `VMM_IDENTITY_WINDOW_LIMIT`
    constraint ADR 0004 flagged as revisit-when-needed, once Milestone
    17's ELF loader and Milestone 18's `task_fork()` had both
    independently hit it. `elf_load()`/`task_fork()` both switched over;
    `vmm.c`'s own page-table bootstrap frames still need the identity
    window (irreducible — they build the tables the direct-map depends
    on). See ADR 0019.

**Testing state:** 19 QEMU smoke tests (`tests/qemu/*.sh`), 4 host unit
test suites (`tests/host/*.c`, run with ASan/UBSan), all passing as of
the last commit. Every milestone has its own dedicated smoke test; run
`make run` for an interactive boot or any `tests/qemu/test_*.sh`
individually for a specific milestone's proof.

**A note on process discipline that held up well:** thirteen milestones
(9-19) all followed the same pattern — implement, boot in QEMU for
real, fix what actually breaks, write the ADR describing what was
tried and what was learned (including dead ends), commit in small
logical pieces. Milestones 10-15 and 17-19 all landed correctly on
the first real boot; Milestone 9 (per-process address spaces) and
Milestone 16 (PS/2 mouse) each hit one genuine bug that needed real
diagnosis (not guessing) to fix — both are documented in detail in
their ADRs (0009, 0016) specifically so the diagnostic *method*, not
just the fix, is preserved for next time something in this territory
breaks. Milestone 19 was also the first since Milestone 8 to need ZERO
marker-text updates in any pre-existing smoke test — a sign the
interface it touched (raw physical-address access) was internal enough
that widening it didn't ripple into anything user-visible.

## Explicitly flagged, NOT started — needs your decision

`CLAUDE.md`'s non-goals list requires flagging these before any work
begins, so none of the following has been touched:

- **A disk driver + real filesystem.** Milestone 13's PCI scan found a
  real PIIX3 IDE controller in QEMU's default machine, so the hardware
  path is there whenever this is wanted. "Real FS" is an explicit
  CLAUDE.md non-goal.
- **ACPI-based shutdown** (as opposed to the reset-only `reboot`
  Milestone 15 already built). Needs ACPI table parsing, which is a
  listed non-goal ("ACPI power mgmt").
- **SMP.** Explicit non-goal.
- **Networking / USB.** Explicit non-goals.

If you want to proceed on any of these, say so explicitly — that's the
signal CLAUDE.md asks for before this territory gets touched.

## Reasonable next steps (not flagged, not started)

These don't touch a non-goal and were the natural next items on the
"build this into an OS" list this session worked through one at a
time:

- **A `sys_exec`-equivalent syscall — flagged here as GENUINELY HARDER
  than it looks, not just another syscall.** Milestone 18 gave a
  running process the ability to fork; it still can't replace its own
  image with a different one, and `elf_load()`
  (`kernel/mm/elf_loader.c`, Milestone 17) doesn't care who invokes
  it — tearing down and rebuilding the calling process's mappings
  (`vmm_for_each_user_page()`-style walk, Milestone 18, or a new
  sibling of `vmm_destroy_address_space()` that resets a region without
  freeing the PML4 itself) is the tractable half. The HARD half,
  discovered while scoping this as this session's next milestone before
  deliberately deferring it: `sys_exec` can never resume through the
  normal `sysretq` epilogue `syscall_entry.asm` always takes, because
  the OLD program (and its stack) is gone — it needs a genuinely NEW
  control-flow primitive, a synchronous mid-syscall resume via `iretq`
  into the freshly loaded image's entry point, built and installed
  entirely within the syscall handler itself (unlike `sys_exit`, which
  sidesteps this by never resuming ANYTHING again, or `sys_fork`, which
  builds its synthetic frame for a DIFFERENT, not-yet-running task).
  Also needs the reset+repopulate sequence to happen with interrupts
  still masked throughout (already true for the whole syscall, but
  worth stating explicitly: a timer tick landing mid-repopulation would
  see a half-built address space) and a full TLB flush (`CR3` reload)
  before ever resuming, since the new mappings can reuse virtual
  addresses the old ones held with DIFFERENT physical frames. Would also
  need at least a second meaningfully-different embedded program to
  select between (there are two now, `hello.asm`/`fork_demo.asm`, but
  neither was written with "being exec'd into" in mind). Worth reading
  ADR 0009's CR3-switch-timing bug again before attempting this — it's
  the closest prior art in this codebase for "state must be fully
  consistent before you can safely resume through it."
- **Remaining memory-maturity items**: VMAs (a real per-process memory
  map instead of a few hardcoded regions), demand paging / copy-on-write
  (Milestone 18's `sys_fork` is a full eager deep copy specifically
  because this doesn't exist yet — see ADR 0018 for why building COW
  wasn't attempted inline with fork itself; Milestone 19's direct-map,
  ADR 0019, would make a page-fault-driven COW handler's own bookkeeping
  simpler to build on top of, once attempted).
- **Synchronization/IPC — now with a concrete motivating case.**
  Milestone 18's `sys_wait` is non-blocking specifically because no
  blocking/sleep-queue scheduler primitive exists (ADR 0018) — a real
  `wait()` needs exactly that, not just IPC in the abstract. Real IPC
  (pipes, shared memory, signals) would additionally matter once
  there's more than one reason for two processes to talk to each other.
- **Cursor/graphics.** Milestone 16 built the mouse *input* path with
  nothing to draw a cursor on. A framebuffer console (flagged as future
  work back in ADR 0008, for UEFI-without-CSM compatibility too) would
  unlock this.

## How to pick this back up

- `make run` — boots the ISO in a GTK window (serial to this terminal).
  `make debug` — same, plus GDB stub on :1234.
- `for t in tests/qemu/test_*.sh; do bash "$t"; done` — full smoke-test
  regression pass. Each also rebuilds first, so this is self-contained.
- Host tests: `gcc -std=c11 -Wall -Wextra -Werror
  -fsanitize=address,undefined -Itests/host/../.. tests/host/test_X.c
  libk/X.c -o /tmp/test_X && /tmp/test_X` for each of `fmt`,
  `heap_alloc`, `ring_buffer`, `elf`.
- Read `/CLAUDE.md` before touching anything — it's the actual
  governing spec for this project (toolchain, safety rules, process
  discipline, the non-goals list above). It overrides default
  assumptions.
- Read `docs/roadmap.md` top to bottom for the full milestone history;
  read the specific `docs/adr/NNNN-*.md` for whichever subsystem you're
  about to touch before changing it (paging, scheduler, interrupts all
  have real prior bugs documented — don't repeat them).
