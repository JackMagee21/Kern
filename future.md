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

## State as of Milestone 22 (2026-08-25)

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

20. **Genuinely blocking `sys_wait()`** — `sys_wait` (`kernel/arch/
    x86_64/syscall.c`) no longer polls: it loops `scheduler_try_wait()`
    then, if nothing matches yet, `sti; hlt; cli` and retries, relying
    entirely on the already-existing preemptive scheduler to give other
    tasks (including the reaper) their turns in between — no new
    `TASK_BLOCKED` state or wake-list needed, since the calling task
    just stays `TASK_READY` in the ordinary ready queue the whole time.
    `kernel/user/fork_demo.asm` now makes exactly ONE `sys_wait` call
    (its old poll-and-spin wrapper deleted) — proof by construction
    that the call genuinely blocks. Found and fixed a real latent bug
    this exposed: `syscall_entry.asm`'s `saved_user_rsp` was a single
    bare global, safe only because syscalls used to be atomic w.r.t.
    scheduling — moved to a per-task `task_t` field plus a
    scheduler-maintained indirection pointer, the same per-task
    redirection pattern already used for `syscall_kernel_rsp`/
    `TSS.RSP0`. A new self-test (`syscall_get_wait_block_count()`)
    proves the blocking path was actually taken, not just that the
    right answer came back by luck — made deterministic (not a timing
    race) by giving the fork demo's child a bounded spin longer than
    the parent's worst-case scheduling delay. See ADR 0020.

21. **Copy-on-write fork** — `sys_fork` no longer eagerly deep-copies
    the parent's address space (ADR 0018's original design): `task_fork()`
    now calls `vmm_fork_cow_page()` (`kernel/mm/vmm.c`) per page, which
    downgrades the PARENT's own existing mapping to read-only+`VMM_FLAG_COW`
    in place and shares the SAME physical frame into the child
    (refcounted via new `pmm_frame_addref()`/`pmm_frame_refcount()`,
    `kernel/mm/pmm.c`). A write from either sibling `#PF`s and is
    resolved by `vmm_handle_cow_fault()` — checked and handled silently
    in `exceptions.c`'s `isr_handler` BEFORE any diagnostic printing,
    confirmed to run with interrupts masked throughout (every exception
    is an interrupt gate, `idt.c`) so it can never race another task's
    fault on the same frame's refcount. Takes the frame over in place
    (no copy) if this is already the last reference — the standard
    real-COW optimization. `kernel/user/fork_demo.asm`'s parent and
    child now write DIFFERENT sentinels to the same originally-shared
    `.data` variable and the parent verifies (strictly after the
    child's own write+exit, via Milestone 20's blocking `sys_wait`,
    reused as this test's own synchronization primitive) that it still
    sees its own value — proof of real isolation, not aliasing. A new
    self-test (`vmm_get_cow_fault_count()`) proves sharing was
    genuinely lazy, not just correct. See ADR 0021.

22. **`sys_exec`** — `task_exec()` (`kernel/sched/task.c`) replaces the
    CALLING process's own running image with a different embedded
    program, reusing the SAME `task_t`/pid/PML4 frame rather than
    creating a new process — the property that distinguishes exec from
    fork+exit, verified via `kernel_main`'s reap-count accounting (5,
    not 6). `vmm_reset_user_address_space()` (`kernel/mm/vmm.c`), a new
    sibling of `vmm_destroy_address_space()` with the OPPOSITE activity
    requirement (must run on the CURRENTLY ACTIVE address space, not an
    inactive one), tears down every existing mapping first; `elf_load()`
    then populates the (now-empty) address space with the new image.
    Resumes through the EXISTING `sysretq` epilogue with zero assembly
    changes — `task_exec()` just overwrites the current syscall's own
    saved frame (`rcx`=new entry, `r11`=fresh RFLAGS, every other GPR
    zeroed) plus the per-task saved user RSP; this was the milestone's
    own real finding — `future.md` had originally flagged `sys_exec` as
    needing a brand new `iretq`-based control-flow primitive, and
    re-reading `syscall_entry.asm` before writing any code found that
    assumption was wrong (see ADR 0022). Two new, genuinely distinct
    embedded programs (`kernel/user/exec_demo.asm`/`exec_target.asm`) —
    deliberately not a reuse of `hello.elf`/`fork_demo.elf`, whose own
    self-tests count their own messages an exact number of times. See
    ADR 0022.

**Testing state:** 22 QEMU smoke tests (`tests/qemu/*.sh`), 4 host unit
test suites (`tests/host/*.c`, run with ASan/UBSan), all passing as of
the last commit. Every milestone has its own dedicated smoke test; run
`make run` for an interactive boot or any `tests/qemu/test_*.sh`
individually for a specific milestone's proof.

**A note on process discipline that held up well:** sixteen milestones
(9-22) all followed the same pattern — implement, boot in QEMU for
real, fix what actually breaks, write the ADR describing what was
tried and what was learned (including dead ends), commit in small
logical pieces. Milestones 10-15, 17-19, and 21-22 all landed correctly
on the first real boot; Milestone 9 (per-process address spaces) and
Milestone 16 (PS/2 mouse) each hit one genuine bug that needed real
diagnosis (not guessing) to fix — both are documented in detail in
their ADRs (0009, 0016) specifically so the diagnostic *method*, not
just the fix, is preserved for next time something in this territory
breaks. Milestone 20 is its own diagnosis story worth naming
separately: the `saved_user_rsp` bug (see item 20 above) was never
observed as a live QEMU failure — it was found by reasoning through
what "another task's syscall can now genuinely interleave" implies
for existing global state, BEFORE writing the fix, matching CLAUDE.md's
"diagnose first, don't guess" discipline applied prospectively rather
than reactively. Milestone 19 was also the first since Milestone 8 to
need ZERO marker-text updates in any pre-existing smoke test — a sign
the interface it touched (raw physical-address access) was internal
enough that widening it didn't ripple into anything user-visible.
Milestone 21 is the first milestone where `#PF` (page fault) became a
genuinely expected, resolved-and-resumed exception rather than always
fatal — verified not just by the self-test passing but by hand-counting
the EXACT number of faults a `-d int,cpu_reset` trace should show (3)
and confirming the trace matched that precise number, not just "some
faults happened and nothing crashed." Milestone 22 is its own kind of
story worth naming too: it's the first milestone where the CORRECT
design turned out to be simpler than what an earlier session (this
same `future.md`, before Milestone 22 started) had predicted was
necessary — re-reading the actual assembly before trusting that
prediction is what found the simpler path, rather than building the
more complex "obviously needed" primitive on faith.

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

- **VMAs — a real per-process memory map instead of a few hardcoded
  regions.** Neither Milestone 21's COW fork (ADR 0021) nor Milestone
  22's `sys_exec` (ADR 0022) needed this (the existing
  `vmm_for_each_user_page()`/PTE-flag approach, and `sys_exec`'s own
  reset-the-whole-private-region approach, were both enough), but any
  FURTHER demand-paging work beyond what those two already do (a
  genuinely on-demand-allocated heap/mmap-equivalent region, precise
  per-region permission tracking, a real path-based `execve` that needs
  to know a region's own bounds) would need one.
- **A real path-based `execve`, once a filesystem exists.** Milestone
  22's `sys_exec` (ADR 0022) can only target one of a small, fixed,
  build-time-embedded set of images (`kernel/sched/task.c`'s
  `exec_lookup_image()`) — there's no filesystem to load an arbitrary
  path from yet. The actual image-swap mechanism
  (`vmm_reset_user_address_space()` + `elf_load()` + overwriting the
  syscall's own saved frame) doesn't need to change for this; only the
  "which bytes" half does.
- **A real sleep-queue/wake scheduler primitive.** Milestone 20 made
  `sys_wait` genuinely blocking (ADR 0020), but deliberately via a
  one-off `sti; hlt; cli` retry loop scoped to just that syscall, not a
  general primitive (no `TASK_BLOCKED` state, no wake-list) — the right
  call for a single caller, but real IPC (pipes, shared memory,
  signals) would need an actual blocked-task/wake-list mechanism once
  there's a second real reason for two processes to synchronize, not
  just "wait for one to exit."
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
