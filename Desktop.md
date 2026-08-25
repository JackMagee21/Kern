# Desktop.md — planning a usable graphical interface

This file is a pre-commitment plan for a multi-milestone arc, written
before any of it is built, the same way `future.md` is a continuation
briefing rather than a design record. It is NOT authoritative once a
milestone actually starts — at that point `docs/roadmap.md` gets the
real entry (deliverables/verification/known limitations) and the
subsystem gets its own `docs/adr/NNNN-*.md`, per this project's
established convention. This file's job is just to write down the
shape of the whole arc, the sequencing logic, and the open questions,
so each milestone along the way has context for why it exists and
what it's building toward.

## The goal, as scoped with the user (2026-08-25)

A multi-window desktop: overlapping windows, focus/z-order, draggable
window chrome, and a small number of genuinely distinct applications —
not just a tech demo of one window. Filesystem stays an explicit
CLAUDE.md non-goal for this entire arc: every app is a build-time-
embedded ELF image launched via `sys_exec` (Milestone 22), the same way
`hello.asm`/`exec_target.asm` already are. No persistent save/open,
no user-installed apps. That's a real, accepted limitation of "usable"
here, not an oversight — revisit only if the FS non-goal itself is
lifted later.

## What already exists (the foundation this arc builds on)

See `future.md` for the full state; the pieces this arc specifically
depends on:
- Per-process address spaces, COW fork, `sys_exec` (Milestones 9, 18,
  21, 22) — real process isolation, so the display server and each app
  are genuinely separate, untrusted-of-each-other processes, not
  threads sharing memory by default.
- A linear graphics framebuffer + 8x8 text console + a movable mouse
  cursor sprite (Milestone 23) — the pixel-plotting primitives
  (`fb_put_pixel`/`fb_fill_rect`/`fb_read_rect`/`fb_scroll_up`,
  `kernel/drivers/framebuffer.h`) this whole arc draws through.
- Real PS/2 keyboard + mouse drivers (Milestones 8, 16) with decoded
  event queues.
- A validated ELF64 loader (Milestone 17) and an `incbin`-based
  embedding convention (`kernel/user/embed/`, just reorganized) for
  packaging a compiled program into the kernel image.

## The one big new decision: a minimal userspace C runtime

Every user program that exists today (`kernel/user/*.asm`) is
hand-written NASM. That was fine for four small, linear demo programs.
It will not scale to a window server (window list, rectangle math,
damage tracking, message parsing) or several real apps — writing and
debugging that much logic in raw assembly, with no local variables
worth the name and no structs, is disproportionately harder and more
error-prone than the actual problems being solved.

So the FIRST piece of new infrastructure this arc needs is a small
freestanding C runtime for ring-3 programs: a `_start` stub (`crt0`)
that calls `main()` then `sys_exit()`s its return value, and thin
wrappers around this kernel's own syscall ABI (`SYS_WRITE`/`SYS_EXIT`/
`SYS_FORK`/`SYS_WAIT`/`SYS_EXEC`, plus whatever IPC syscalls later
milestones add) so application code can call `sys_write(...)` instead
of hand-rolling `mov eax, 1 / syscall` in every program.

**This is explicitly NOT the "POSIX userland" CLAUDE.md non-goal.**
That non-goal means not building POSIX compatibility (POSIX file I/O,
signals, the POSIX process/threading APIs, `libc` as in glibc/musl).
This is a small, custom runtime for THIS kernel's own small, non-POSIX
syscall numbers — closer in spirit to `/libk` (freestanding, no hosted
headers, host-testable where the logic doesn't need privileged
context) than to a real libc. Flagged here explicitly, as CLAUDE.md
asks for any new dependency/runtime, so it's a visible decision rather
than something that crept in silently. Lives in a new directory
(`kernel/user/rt/`, created when this milestone actually starts, not
before) — separate from `/libk`, since `/libk` is kernel-side code by
its own stated purpose.

## Milestone sequence

Each of these becomes its own real milestone (implement, boot-test,
ADR, commit) when reached — this is the planned ORDER and the reason
each one is needed now, not a deliverables list.

1. **Minimal userspace C runtime — DONE (Milestone 24, ADR 0024).**
   `crt0` + syscall wrappers + a tiny freestanding string/memory helper
   subset. Proven by rewriting one existing demo (`hello.asm` →
   `hello.c` using the runtime) and confirming byte-identical behavior
   against the existing smoke tests, zero assertion changes needed.
2. **A general blocking/wake scheduler primitive — DONE (Milestone 25,
   ADR 0025).** `TASK_BLOCKED` + `scheduler_block_current()`/
   `scheduler_wake()`, generalizing Milestone 20's one-off
   `sti;hlt;cli` retry loop. No searchable wake-list turned out to be
   needed (see ADR 0025) — a waker always already holds the specific
   task to wake. `sys_wait` itself was deliberately NOT rewired onto
   this yet (no concrete benefit needed for that on its own); Milestone
   26's IPC is the primitive's first real consumer.
3. **An IPC primitive — DONE (Milestone 26, ADR 0026).** Message
   passing (`kernel/ipc/msgqueue.c`, a per-task inbox) and named
   shared-memory objects (`kernel/ipc/shm.c`) between two processes,
   proven end to end by a sender/receiver demo pair. Turned out
   narrower than general VMA tracking after all — a small, fixed-
   capacity named-object table was sufficient, so `future.md`'s
   long-deferred VMA item stays deferred; cleanup reuses the EXISTING
   COW frame-refcounting (ADR 0021) rather than new machinery. Needed a
   new `pid -> task_t*` registry (`scheduler_find_task()`) since a
   blocked IPC destination isn't searchable in the ready queue. Four
   real bugs found and fixed — see ADR 0026.
4. **A minimal display server, one client, no overlap.** The actual
   hard-unknown milestone: prove the client-server model works at all.
   One server process owns the framebuffer; a client asks for a
   rectangle; the server enforces the bound. Deliberately no multi-
   window logic yet — isolate this risk before building UI on top of
   it.
5. **Multiple windows: z-order, damage tracking, input focus.** Window
   list, routing keyboard/mouse events (including clicks — the cursor
   currently only tracks movement, buttons aren't wired to anything)
   to whichever window is focused.
6. **Window chrome and basic widgets.** Draggable/closable title bars,
   at least one interactive control. This is where it starts feeling
   like a desktop rather than a windowing demo.
7. **Real applications.** A small number of genuinely different
   programs (not just tech-demo processes) — candidates: a clock, a
   simple text/log viewer, something interactive enough to prove input
   routing actually works end to end.

## Verification approach (per layer)

Consistent with every milestone so far: an actual QEMU boot, not "it
compiled." The framebuffer-era technique from Milestone 23 — a real
QEMU `screendump`, visually inspected once and then turned into an
automated pixel-position assertion — extends naturally to windows
(assert a window's chrome pixels are where expected) and click routing
(inject a real synthetic mouse click via the QEMU monitor, assert the
right client received it). IPC/blocking-wake milestones get their own
counters proving the new code path was actually exercised (the same
"prove it was exercised, not just correct by luck" pattern
`syscall_get_wait_block_count()`/`vmm_get_cow_fault_count()`/
`syscall_get_exec_count()` already established).

## Non-goals reaffirmed for this whole arc

Unchanged from `CLAUDE.md`, still requiring the user's explicit
go-ahead before any of this arc touches them: a real filesystem, ACPI
power management, SMP, networking/USB. A GUI is tempting territory to
quietly want these (persistent settings, multiple cores for
compositing) — reaffirmed here specifically so that temptation doesn't
turn into silent scope creep partway through the arc.

## How this file gets used

Each milestone above starts only once the previous one is DONE and
verified — same "one subsystem per change" discipline as every
milestone before it. As each one starts, its real deliverables/
verification/known-limitations get written into `docs/roadmap.md` and
a `docs/adr/NNNN-*.md`, same as always; this file doesn't get expanded
into that level of detail; it just gets a line updated (or, once the
whole arc is done, folded into `future.md`'s own state summary).
