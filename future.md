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

## State as of Milestone 35 (2026-08-26)

**GUI arc COMPLETE** — see `Desktop.md` for the full multi-milestone
plan (multi-window desktop, filesystem staying a non-goal, confirmed
with the user). Milestones 24-31 and 33 (below) are all seven of
Desktop.md's own numbered items; Milestones 32, 34, and 35 are real
follow-on work outside the arc's own numbering (all see their own
entries below and `docs/roadmap.md`).

Milestone 32 also got a real follow-up fix this session: a genuine
`#GP` under KVM while actually dragging a window, root-caused to the
SS-fixup only being wired into the timer/exception paths, not the
generic IRQ dispatcher every OTHER interrupt (keyboard, mouse) goes
through. Fixed and verified with a real drag-storm repro under KVM —
see ADR 0032's own Addendum section.

**Real KVM acceleration is now safe and enabled** (`make run`/`make
debug`, when `/dev/kvm` is accessible) — Milestone 32 found and fixed a
real, hardware-only ring-3 SS-corruption bug that made it unsafe
before. `make run` now grabs the mouse automatically (GTK
`grab-on-hover=on`, forced onto XWayland via `GDK_BACKEND=x11` — WSLg's
own native-Wayland relative-pointer support is unreliable) for
Milestone 31's own draggable windows.
As of Milestone 31, per the user's own explicit request, booting
through the `[OK]` self-checks leads into a real, interactive GUI: two
windows with title bars, draggable and closable with the mouse.
Milestone 33 added a THIRD window that stays alive and keeps redrawing
itself forever — the first genuinely persistent, self-updating
application this kernel has ever run, completing Desktop.md's own final
GUI-arc item.

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

23. **Graphics framebuffer console and mouse cursor** — Milestone 16's
    mouse driver finally has something to draw a cursor on. A real
    design fork (VGA text mode and a linear framebuffer can't run
    simultaneously) was checked with the user before writing code; the
    chosen scope replaced the whole VGA console (`kernel/drivers/vga.c`,
    retired) with a real framebuffer-rendered one
    (`kernel/drivers/fbconsole.c`, an embedded public-domain 8x8 font)
    rather than a narrower graphics-mode-only demo. `kernel/drivers/
    framebuffer.c` negotiates the mode via a new Multiboot2 request tag
    (`kernel/arch/x86_64/boot.asm`) and reaches its physical memory via
    the EXISTING Milestone 19 direct-map — zero new page-table work,
    since QEMU's framebuffer BAR sits well under 4GiB. `kernel/drivers/
    cursor.c` moves a sprite via real mouse deltas from a SECOND,
    independent event queue in `mouse.c` (found and designed around
    before it could break `test_mouse_selftest.sh`'s existing contract,
    not discovered by a live failure). Verified via a real QEMU
    screendump, visually inspected (font rendering was correct on the
    first attempt) AND turned into an automated pixel-position
    assertion (`test_framebuffer_selftest.sh`) around a real injected
    `mouse_move`. See ADR 0023.

24. **Minimal userspace C runtime** — the first step of the GUI arc
    (`Desktop.md`). `kernel/user/rt/` (`crt0.asm` + `syscall.h/.c` +
    `string.h/.c`) lets ring-3 programs be written in ordinary C instead
    of hand-rolled NASM, proven by rewriting `hello.asm` in place as
    `hello.c` — byte-for-byte identical behavior, zero test assertion
    changes anywhere. `Makefile` gained a separate `USER_CFLAGS`
    (`-mcmodel=large`, since process-private code is architecturally
    stuck above `0x8000000000` — outside small/kernel-model addressing
    range; no `-mno-red-zone`, since ring-3 code never takes a trap on
    its own stack, ADR 0007). Explicitly NOT the "POSIX userland"
    non-goal — a small wrapper library for this kernel's own six
    syscalls, nothing resembling POSIX. No new smoke test file was
    needed -- `test_elf_loader_selftest.sh`/`test_ring3_syscall_
    selftest.sh` (both extended with a documentation note, ADR 0024)
    already assert on `hello`'s exact output, which the C rewrite had
    to match byte-for-byte; a new test would have been strictly weaker
    proof than that. See ADR 0024.

25. **General blocking/wake scheduler primitive** — `TASK_BLOCKED`
    (`kernel/sched/task.h`) plus `scheduler_block_current()`/
    `scheduler_wake()` (`kernel/sched/scheduler.c`) generalize
    Milestone 20's one-off `sys_wait`-specific polling loop: a blocked
    task now genuinely leaves the ready queue (zero wasted turns) until
    another task explicitly wakes it, verified via a deterministic
    (explicit handoff flag, not a tuned delay) two-kernel-thread
    self-test. Two real bugs found and fixed this milestone — the first
    live-boot bug since Milestone 16: a raw `pushfq` held open across
    intervening C code in `scheduler_wake()`'s first draft (caught in
    review before booting) and, the actual boot hang, `scheduler_block_
    current()`'s "returns with IF=0" contract being wrong for a KERNEL
    THREAD caller with no syscall return path to re-enable interrupts
    for it — diagnosed from the tell that the ENTIRE machine froze, not
    just this one test, then fixed with the same explicit `sti`
    `scheduler_exit_current()` already uses before its own trailing
    loop. `sys_wait` itself is NOT yet rewired onto this primitive
    (deliberate scope boundary — no concrete benefit needed yet). See
    ADR 0025.

26. **IPC message passing and shared memory** — two genuinely isolated
    processes (`kernel/user/ipc_sender.c`/`ipc_receiver.c`) exchange a
    real message (`kernel/ipc/msgqueue.c`, a per-task inbox +
    `scheduler_block_current()`'s first REAL consumer) and a real block
    of shared physical memory (`kernel/ipc/shm.c`, named objects —
    deliberately narrower than general VMA tracking, whose actual
    trigger turned out narrower than `future.md` had speculated).
    Cleanup reuses `pmm.h`'s existing COW refcounting (ADR 0021) rather
    than a new destroy API. A new `pid -> task_t*` registry
    (`scheduler_find_task()`) was needed since a blocked IPC
    destination isn't in the ready queue to search. Four real bugs
    found and fixed this milestone (three from live boot symptoms, one
    caught in review) — see ADR 0026 for the full diagnostic trail,
    including a genuine contradiction in the log (sender reported
    failure, receiver still verified success) that pointed straight at
    an inverted return-value check, and a scheduling-order assumption
    that turned out backwards (`scheduler_add_task()` makes the LAST
    task added run FIRST, not the first). See ADR 0026.
27. **Minimal display server, one client, no overlap** — the GUI arc's
    own flagged "actual hard-unknown milestone": prove the
    client-server display model works at all before any multi-window
    logic goes on top. Two new syscalls (`SYS_FB_ACQUIRE`/
    `SYS_FB_PRESENT`, `kernel/arch/x86_64/syscall.c`) — the kernel
    stays the sole framebuffer owner/pixel-writer (ownership is
    kernel-ENFORCED by pid, not a userspace convention), but the actual
    canvas-size POLICY — "the server enforces the bound" — lives in
    userspace (`kernel/user/display_server.c`'s own fixed 200x150
    maximum). A tiny 3-message protocol
    (`kernel/user/display_protocol.h`) layered entirely on Milestone
    26's existing IPC/shm mechanism, no new IPC machinery. The client
    (`kernel/user/display_client.c`) deliberately asks for 400x300 and
    gets 200x150 back — proven pixel-for-pixel by a real QEMU
    `screendump` bounding-box check
    (`tests/qemu/test_display_server_selftest.sh`), not just a
    self-reported marker. Booted clean on the FIRST real attempt — every
    design question (including a causal-ordering argument for why
    Milestone 26's own scheduling-order bug class couldn't recur here)
    was worked through in review before ever running QEMU. See ADR 0027.
28. **Multiple windows and z-order compositing** — extended Milestone
    27's server to two clients (`display_client_a.c`/`_b.c`), cascaded
    so their canvases genuinely overlap; correct z-order needed no new
    compositing machinery at all, just strict presentation order (both
    windows are fully opaque). `Desktop.md`'s own milestone 5 originally
    bundled this with real click-driven input focus; split those apart
    deliberately (CLAUDE.md: one subsystem per change) since routing a
    real hardware input event to a ring-3 process is genuinely separate,
    unbuilt subsystem work — its own later milestone now. Found TWO
    real bugs neither planned in advance: the smoke test's own
    hardcoded absolute screen coordinates broke because
    `fbconsole.c`'s `fb_scroll_up()` shifts the ENTIRE framebuffer
    (already-drawn windows included) once this milestone's extra boot
    output pushed a scroll threshold no earlier milestone had reached —
    fixed the TEST to check geometry relative to the windows' own
    discovered position; the SAME extra scroll exposed a genuine,
    pre-existing ghost-trail bug in Milestone 23's mouse cursor
    (`cursor.c` had no way to know a scroll had happened, so its next
    redraw restored stale save/restore data) — fixed with new
    `cursor_hide()`/`cursor_show()` functions wrapped around the
    scroll call, a real cross-module coupling, not a workaround. See
    ADR 0028 — including an explicitly flagged, NOT-yet-fixed
    limitation: windows themselves (unlike the cursor) are still not
    immune to later console scroll, a real architectural gap worth
    addressing before chrome/interactivity makes it user-facing.
29. **Real input-driven click routing** — the genuinely-new-subsystem
    half of milestone 5's split: a real PS/2 left-click, decoded by an
    actual IRQ12 report, delivered via IPC to a specific ring-3
    process (`kernel/user/input_focus_demo.c`, a standalone proof-of-
    mechanism demo, RETIRED the next milestone once something real
    consumed it — see item 30) for the first time this kernel has ever
    done so. New `SYS_INPUT_SUBSCRIBE` syscall + a small
    `kernel/drivers/input_router.c`, with click-EDGE detection living
    in `cursor.c` (already sees every event's button level, the
    natural place to turn it into a transition). A real structural
    conflict was found and fixed IN REVIEW, before ever booting: a
    process that blocks forever for external input can't be part of
    `kernel_main`'s own deterministic reap-count gate, or every OTHER
    headless test would hang forever — fixed by creating the demo
    BEFORE the frame-leak baseline, alongside the permanent kernel
    threads rather than the bounded demo processes. See ADR 0029.
30. **Real click-driven window raising, and scroll-immune windows** —
    wires Milestone 29's mechanism into an actual visible effect:
    `display_server.c` redesigned from "serve N clients then exit"
    into a genuinely persistent process that raises a window on a real
    click (retiring Milestone 29's own standalone demo — an
    unresolvable subscription-exclusivity conflict otherwise). Also
    delivered the REAL fix for Milestone 28's own flagged, deferred gap
    (windows drifting from console scroll) — this milestone's own
    hit-testing turned that from cosmetic into a genuine functional bug
    (a click at a window's REAL, drifted position would miss its
    NOMINAL hit-test rect). `fb_scroll_up()` gained a bounded
    `region_height`; windows relocated to y ≥ 480, permanently outside
    the console's own reserved scroll region. Found and fixed two MORE
    real bugs verifying all this, neither planned: a glyph-draw/cursor
    corruption, and — the real prize — the actual root cause of a
    genuine screendump-visible corruption: `console_putc()`'s shared
    cursor state had NO mutual exclusion against preemption, so two
    ring-3 processes' interleaved `sys_write()` calls could corrupt it
    once enough processes existed to make the race likely (fixed with a
    short interrupt-disabled critical section, the same save/restore-
    flags idiom `scheduler_wake()` established, Milestone 25). Proven
    with a SECOND QEMU screendump showing the overlap region genuinely
    flip ownership after a real injected click. Reap-count target
    LOWERED for the first time ever, 10 → 9. See ADR 0030.
31. **Window chrome and basic widgets** — the milestone Desktop.md
    itself calls "where it starts feeling like a desktop rather than a
    windowing demo," and where the user's own explicit request (boot
    through the `[OK]` checks, then a real usable GUI) landed.
    `display_server.c` gained a server-drawn title bar (composited
    ABOVE each client's own canvas — the client needed ZERO changes,
    chrome is entirely server-owned so a client can't fake/omit its own
    decorations) with a real close button. Two new input events
    (`INPUT_EVENT_DRAG`, sent only while the button is held AND moving;
    `INPUT_EVENT_RELEASE`, the press edge's mirror) let a real mouse
    drag actually move a window, tracked entirely server-side. Proven
    with genuine QEMU-injected input, not a self-report:
    `test_window_chrome_selftest.sh` drags one window 450px and
    confirms its EXACT new position via screendump, then closes the
    other and confirms it vanished completely while the dragged one
    stayed intact. One existing test's pixel math needed a real,
    hand-derived correction (not a loosened tolerance) for a genuine
    consequence of chrome: one window's title bar now also covers part
    of a neighbor's exposed canvas. See ADR 0031.
32. **Real-hardware-only ring-3 SS corruption, and safe KVM
    acceleration** — not a GUI-arc item; a core correctness fix found
    investigating Milestone 31's own reported drag lag. KVM was the
    actual performance fix needed, but booting under it immediately
    exposed a real, reproducible `#GP` that TCG had silently never
    reproduced across 30+ milestones, back to Milestone 18/21's
    original fork/COW-fault code. Root-caused with hard evidence: a
    defensive check added to `timer_tick_handler` did NOT catch the
    fault, ruling out the obvious hypothesis before chasing it further;
    a new, permanently-kept flight recorder
    (`scheduler_record_switch_diag()`) is what actually found it — a
    real hardware capture of a ring-3 COW `#PF` frame losing SS's RPL
    bits (`0x23` → `0x20`) on a path TCG never reproduces. Fixed
    defensively (`trap_frame_fixup_ss()`), not by chasing the exact
    KVM/VT-x mechanism: this kernel's own architecture only ever uses
    two possible SS values, so re-asserting the correct one is provably
    safe regardless of the hypervisor's own reason for the corruption.
    `make run`/`make debug` now enable KVM conditionally (checked at
    `make` time, not hardcoded). See ADR 0032.
33. **Real applications** — Desktop.md's own final GUI-arc item,
    completed. `kernel/user/pulse_app.c`, a THIRD window, spatially
    disjoint from clients A/B so no existing exact-pixel test needed to
    change, proves the one property no earlier client ever had to:
    staying alive and genuinely changing its own on-screen content
    forever, not just presenting once and exiting. One new no-fields
    protocol message (`DISPLAY_OP_REDRAW`) was all the server needed —
    `composite_all()` already re-reads every window's current content
    from scratch, so no new per-window bookkeeping was required. Paced
    with a plain `sys_nop` spin (matching `fork_demo.asm`'s own
    bounded-loop precedent) cycling a small fixed color palette.
    `raise_to_top()` generalized from a hardcoded 2-window swap to a
    real shift loop, exposed as genuinely wrong (not just narrow) once
    `WINDOWS_TOTAL` became 3. Self-test accounting changed for real,
    derived reasons: `sys_fb_present`'s count check moved from exact
    equality to a floor (background redraws now happen on a schedule no
    self-test can pin down exactly), and the frame-leak baseline's
    permanent-deficit constant grew by a real, hand-derived 15 pages (the
    pulse app's own canvas, held forever because IT never exits — a
    different reason than clients A/B's own 60-page deficit, which
    exists because the SERVER never drops its reference to theirs).
    Proven with a genuinely new kind of smoke test
    (`test_pulse_app_selftest.sh`): since the server's `DISPLAY_OP_REDRAW`
    handler deliberately logs nothing, there's no serial marker for "a
    redraw happened" — the test instead polls real repeated screendumps
    until the sampled pixel's color genuinely changes, the first test in
    this suite to synchronize on pixel state directly rather than a log
    line. One real regression found and fixed during verification: an
    early palette choice's near-red entry visually collided with
    `test_framebuffer_selftest.sh`'s existing whole-screen cursor-color
    scan (built when only an 8x8 sprite could ever be that shade) —
    caught by actually running the full regression suite, not assumed
    safe in advance. See ADR 0033.

34. **A real client exit/close protocol** — not part of Desktop.md's
    own arc (already complete), the top item this file's own
    "Reasonable next steps" section flagged. `DISPLAY_OP_EXIT`
    (`display_protocol.h`, opcode 7) tells a window's owning client to
    actually exit when its close button is clicked, using a new
    non-blocking `sys_ipc_try_recv` syscall (exposing the
    already-existing `ipc_try_recv()` kernel primitive, which
    `sys_ipc_recv`'s own doc comment had explicitly flagged as YAGNI
    until something needed it) so the pulse app's animation loop can
    poll for it once per frame without giving up its own pacing to
    block waiting for a close that might never come. No reaper/
    scheduler changes needed — the pulse app was already an ordinary
    orphan process, so `sys_exit` and the existing reaper already
    handle its exit the same way they handle every other process's.
    Verified with a new smoke test confirming three independent facts
    (the server's close marker, the CLIENT's own exit-received marker,
    and a real reap marker after the shell prompt) plus a clean real-KVM
    boot through the identical sequence — deliberately exercised under
    KVM since this is exactly the code path ADR 0032's own bug class
    lived in. See ADR 0034.
35. **A real clock app** — a fourth window
    (`kernel/user/clock_app.c`), rendering real `HH:MM:SS` via a new
    `sys_rtc_read` syscall (a thin wrapper around the already-existing
    `rtc_read()` — port I/O is ring-0-only, so this is the only way
    ring-3 code can read real time) and a small self-contained 3x5-pixel
    digit font, deliberately not built on `fbconsole.c`'s own
    console-shaped font machinery. Only redraws when the displayed
    second actually changes. Joins the go-signal chain one link further
    (A → B → pulse app → clock app) and supports `DISPLAY_OP_EXIT`
    (Milestone 34) from the moment it was created, not retrofitted —
    proof that mechanism generalizes to a second persistent client.
    Verified with a new smoke test: two real screendumps three seconds
    apart confirm the digits genuinely change (not a static image),
    then a real close-click confirms the same three-fact exit proof
    Milestone 34 established. Booted clean on the first real attempt;
    clean under real KVM through the identical close/exit/reap
    sequence. See ADR 0035.

**Testing state:** 30 QEMU smoke tests (`tests/qemu/*.sh`), 4 host unit
test suites (`tests/host/*.c`, run with ASan/UBSan), all passing as of
the last commit. Almost every milestone has its own dedicated smoke
test (Milestone 24 is the one exception — see item 24 above for why
reusing two existing tests unchanged was strictly stronger proof); run
`make run` for an interactive boot or any `tests/qemu/test_*.sh`
individually for a specific milestone's proof.

**A note on process discipline that held up well:** twenty-nine
milestones (9-35) all followed the same pattern — implement, boot in
QEMU for real, fix what actually breaks, write the ADR describing what
was tried and what was learned (including dead ends), commit in small
logical pieces. Milestones 10-15, 17-19, 21-22, 24, 27, 29, 31, 34, and
35 all landed correctly on the first real boot (Milestone 29's own real find
— a process blocking forever for external input would hang every OTHER
test's reap-count gate — was caught in review, before ever booting, not
from a live failure); Milestone 9 (per-process address spaces),
Milestone 16 (PS/2 mouse), Milestone 25 (blocking/wake primitive),
Milestone 26 (IPC/shm), Milestone 28 (multi-window z-order — its own
demo logic booted clean, but exposed a real, pre-existing latent bug in
Milestone 23's mouse cursor via a pre-existing test, not a new one
written to find it), and Milestone 30 (window raising — its own core
redesign booted clean, but verifying it surfaced a genuine
screendump-visible corruption whose real root cause turned out to be a
pre-existing, unguarded concurrency race in the console driver shared
by every ring-3 process's `sys_write()`, exposed rather than caused by
this milestone's own changes — see ADR 0030's Decision for the full
diagnostic trail, including a rejected first fix that only made the
corruption worse before the real cause was found) each hit real bugs
that needed
real diagnosis (not guessing) to fix — all are documented in detail in
their ADRs (0009, 0016, 0025, 0026) specifically so the diagnostic
*method*, not just the fix, is preserved for next time something in
this territory breaks. Milestone 26 found FOUR separate bugs in one
milestone, its own kind of record so far — worth naming because each
was found a DIFFERENT way: an inverted return-value check caught by
noticing a genuine contradiction in the log (sender reported failure,
receiver still verified success — impossible unless the check itself
was wrong); a frame leak caught by the process-lifecycle self-test's
own exact-baseline assertion; a scheduling-order assumption
(receiver-blocks-first) that turned out backwards, caught by the
blocking-path self-test's own zero-count panic; and a shm-VA-reset bug
in `task_fork()` caught in review, reasoning through what the EXISTING
COW mechanism would do to a NEW field, before ever booting. Milestone
25's own bug is worth naming specifically too: the boot didn't just
fail one assertion, it froze SOLID (every unrelated task's progress
stopped dead) — recognizing that a total, machine-wide freeze (not an
isolated test failure) was the actual diagnostic signal is what pointed
straight at "interrupts got permanently disabled" rather than a logic
bug local to the new test. Milestone 20 is its own diagnosis story worth naming
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
more complex "obviously needed" primitive on faith. Milestone 23 hit a
real design fork worth naming too: the two-consumers-one-queue mouse
bug (cursor_poll() vs. the shell's `mouse` command) was found by
reading `shell.c`'s existing code before writing `cursor.c`, never
observed as a live test failure -- the same prospective-diagnosis
pattern Milestone 20 established. Separately, its embedded bitmap
font's own extraction script had a real, single-glyph parsing bug
(a source comment's stray `{` character) caught by validating every
row's byte count before ever embedding it, not by a garbled on-screen
character -- CLAUDE.md's "verify against a real source, don't guess"
discipline applied to a cosmetic-only concern for the first time.

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

`Desktop.md`'s GUI arc (Milestones 24-31 and 33; Milestone 32 was a core
correctness fix outside the arc's own numbering) is now COMPLETE — all
seven of Desktop.md's own items are done. Milestone 34 (real client
exit/close protocol, ADR 0034) and Milestone 35 (a real clock app, ADR
0035) closed the two concrete items this section used to flag. There is
no single obvious "next arc" the way the GUI arc was; what remains:

- **Dynamic window creation** (spawning a NEW window/program instance
  from the running shell, rather than every window being a fixed,
  compile-time `WINDOWS_TOTAL` slot created once at boot). Would need a
  general N-window `raise_to_top()`/hit-test (Milestone 33 already
  generalized the shift logic, just not the fixed-slot array sizing) and
  some real "launch a program" shell command.
- A real path-based `execve` remains blocked on the filesystem non-goal.

A few smaller items outside that arc, not touching a non-goal:

- **Cosmetic polish on Milestone 23's graphics console.** The mouse
  cursor is a plain filled square (ADR 0023's Known limitations), not a
  real arrow/pointer shape; the framebuffer console has no blinking
  text-input caret the way VGA text mode did. Neither is a correctness
  gap -- both were deliberately deferred as cosmetic-only.
- **Update `docs/roadmap.md`'s Milestone 8 text** to note VGA text mode
  was retired in favor of Milestone 23's framebuffer console, if a
  future reader finds the "known limitation: doesn't work on UEFI
  without CSM" line there confusing now that it's moot (superseded, not
  fixed) -- a documentation nit, not a code change.

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
