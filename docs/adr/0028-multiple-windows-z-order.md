# ADR 0028: multiple windows and z-order compositing

## Status
Accepted and verified — `make run` boots the real ISO; two genuinely
separate clients (`kernel/user/display_client_a.c`/`display_client_b.c`)
each get a canvas from `kernel/user/display_server.c`, cascaded so
they genuinely overlap (a real 150x100 shared region), and the second
client's window is provably composited ON TOP of the first's in that
region — a real QEMU `screendump` confirms this pixel-for-pixel, not
just that both processes' own success markers printed. A real,
pre-existing latent bug in the mouse cursor (Milestone 23) was found
and fixed along the way — see Verification. All twenty-six smoke tests
and all four host test suites pass. `-d int,cpu_reset` trace unchanged
from Milestone 27 (1 `#BP`, 3 `#PF`, zero kernel-caused resets). Booted
clean on the first real attempt for the actual demo logic; one real bug
(the cursor/scroll interaction, below) was found via the pre-existing
`test_framebuffer_selftest.sh` once this milestone's own extra boot
output pushed a scroll threshold no earlier milestone had reached.

## Context
Fifth step of the GUI arc (`Desktop.md`). `Desktop.md`'s own milestone
5 bundles three things together: "z-order, damage tracking, input
focus." This ADR covers only the FIRST of those — see Decision for why
the other two were deliberately split into their own later milestone
rather than attempted together here.

## Decision

- **Scope split: z-order compositing now, real input-driven focus
  later.** `Desktop.md`'s milestone 5 bundles z-order + damage tracking
  + input focus as one arc item, written speculatively before any of it
  was built. Wiring a real mouse CLICK to route to a specific ring-3
  process is genuinely separate subsystem work from window
  compositing — it needs a new mechanism for delivering a hardware
  input event to userspace (nothing in this kernel does that yet;
  `kernel/drivers/mouse.c`'s existing consumers are all kernel-side,
  `cursor_poll()`/the shell's own `mouse` command) — not just an
  extension of the client-server protocol Milestone 27 already proved.
  CLAUDE.md's "one subsystem per change": conflating "does compositing
  work" with "does hardware-input-to-userspace-IPC work" in one change
  would make a failure ambiguous about which subsystem broke. Z-order
  compositing (this milestone) is scoped narrowly enough to build on
  Milestone 27's mechanism directly, with no new kernel-side machinery
  at all; real click-driven focus/raising is deferred to its own
  later milestone (`docs/roadmap.md`'s placeholder, updated below).
- **Two clients, not a general N-window server.** `MAX_CANVAS_W/H`
  policy and a fixed 2-slot cascade placement
  (`window_x`/`window_y[WINDOWS_TOTAL=2]`, `display_server.c`) — enough
  to prove real spatial overlap and occlusion, without building a
  dynamic window list this milestone doesn't need yet (that's real
  work for whichever later milestone adds move/close/raise).
- **The server serves clients in a flat, strictly sequential loop** —
  recv REQUEST, send GRANT, recv PRESENT, composite, send ACK, THEN
  move to the next client — rather than a general per-pid state
  machine that could handle clients' messages arriving in any
  interleaved order. This is only correct because the CLIENTS
  themselves guarantee the order: client B's own `DISPLAY_OP_REQUEST`
  cannot reach the server until client A's canvas is confirmed already
  composited (see the next point). Pushing that ordering guarantee onto
  the (simpler, symmetric) client side kept the server's own loop
  trivial to read and reason about — the general state-machine
  alternative was rejected as unneeded complexity for exactly 2,
  fully-cooperating demo clients (CLAUDE.md: don't build for a
  hypothetical general case the actual consumers don't need).
- **A new `DISPLAY_OP_ACK` (server → client) closes a real causality
  gap the first design draft had.** The very first draft had client A
  send client B a `DISPLAY_OP_GO` signal immediately after its own
  `sys_ipc_send(DISPLAY_OP_PRESENT)` call returned — but `sys_ipc_send()`
  only proves a message was ENQUEUED in the server's inbox, never that
  the server has actually finished (or even started) acting on it.
  Caught in review, before ever booting, by re-deriving what
  `ipc_send()`'s actual contract is (`kernel/ipc/msgqueue.c`, Milestone
  26) rather than assuming "sent" means "processed": the server now
  sends an explicit `DISPLAY_OP_ACK` only AFTER `sys_fb_present()` has
  genuinely returned, and client A waits for that ack before ever
  signaling client B — turning "my canvas is really on screen" into an
  observable fact the go-signal's own correctness actually depends on,
  not an assumption.
- **A new client → client message.** `DISPLAY_OP_GO` is the one message
  in this protocol two CLIENTS send each other directly, not
  client↔server — proving `sys_ipc_send`/`sys_ipc_recv` were never
  actually restricted to client-server traffic, just addressed by pid
  like anything else.
- **Windows are fully opaque, so correct z-order needs no compositing
  pass at all — just presentation ORDER.** Since neither window has
  per-pixel transparency, painting each one in turn (bottom z-order
  first) naturally makes a later window's pixels win in any
  overlapping region. No background clear, no dirty-rectangle/damage
  tracking, no separate "recomposite everything" function exists in
  this server at all — deliberately simpler than a real compositor,
  sufficient because nothing in this milestone ever moves, closes, or
  reorders a window after it's first drawn (see Rejected alternatives
  for why a full damage-tracking pass was rejected).

## Rejected alternatives
- **A general per-pid server state machine**, to tolerate clients'
  messages arriving in arbitrary interleaved order. Rejected — see
  Decision; the two demo clients' own go-signal hand-off already makes
  strict sequential order a real, provable invariant, not an
  assumption, so the extra machinery would be unused complexity.
- **Real damage-rectangle tracking / a background-clear-and-recomposite
  pass.** Rejected for this milestone: nothing here ever moves, closes,
  or re-orders a window after its first draw, so "paint once, in
  z-order, and never again" is sufficient and correct. Real damage
  tracking is future work for whichever milestone adds window
  move/close/raise (`Desktop.md`'s own next arc item).
- **Wiring real mouse clicks to route to the server, in this same
  milestone.** Rejected — see Decision's scope-split reasoning. Its own
  later milestone.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-27
  marker unchanged, plus: both clients' own markers, `[OK] display
  server: presented window 0x0` / `0x1` (in that exact order, checked
  by the smoke test), `[OK] display server: all windows presented in
  z-order`, and `[OK] display server self-test passed, sys_fb_present
  blitted 0x2 frame(s)...` — `kernel_main`'s own self-test now checks
  for EXACTLY 2, not just ">0" (Milestone 27's own weaker check), since
  this milestone knows precisely how many should have happened.
- **`tests/qemu/test_display_server_selftest.sh` (rewritten for this
  milestone)**: a real QEMU `screendump`, taken after the shell prompt
  appears, is scanned for BOTH clients' distinctive fill colors (teal
  for A, orange for B). Client B's (topmost) bounding box must be its
  full, unbroken 200x150/30000-pixel rectangle; client A's must be
  reduced to an L-shape (30000 - 15000 = 15000 pixels, the overlap
  region client B now owns) with the SAME outer extent as before — a
  bounding box alone can't see a missing corner, so the test ALSO
  spot-checks specific points: client A's own untouched corners must
  still be teal, while the corner both windows cover must be orange,
  not teal — direct, pixel-level proof of occlusion direction, not just
  "both colors exist somewhere."
- **A real, unplanned finding: window position (and this test's own
  first draft) turned out to be coupled to unrelated console text
  volume.** The test's first draft checked ABSOLUTE screen coordinates
  (100,100)/(150,150) — it failed, showing both windows shifted up by
  exactly 32 pixels. Root-caused (not guessed) by directly inspecting
  the actual screendump pixels around the expected region: both
  windows' TOP rows had shifted, uniformly, by the same amount.
  `kernel/drivers/fbconsole.c`'s own `fb_scroll_up()` (Milestone 8/23)
  shifts the ENTIRE framebuffer once the console's visible rows fill up
  — this milestone's own extra boot-time output (each client and the
  server print several new lines) pushed the total console output past
  that threshold MORE than Milestone 27's single-client demo did,
  producing more scroll events, which shifted the ALREADY-DRAWN windows
  along with everything else. Fixed the TEST (not the windows
  themselves — see Known limitations) by checking geometry RELATIVE to
  wherever client A's own top row actually landed (x is still checked
  absolutely, since a vertical-only scroll can never move it) instead
  of a hardcoded absolute y — a strictly more robust check of the same
  real claim (relative cascade offset, size, occlusion direction), not
  a weaker one.
- **A real, second bug the same investigation surfaced: a genuine
  ghost-trail regression in `kernel/drivers/cursor.c` (Milestone 23),
  caught by that milestone's OWN pre-existing
  `test_framebuffer_selftest.sh` check** (not a new test written for
  this ADR). The same extra scroll volume that shifted the windows also
  shifted the mouse cursor's own already-drawn sprite — but
  `cursor.c` had no way to know a scroll had happened, so its next
  real erase/redraw (triggered by `test_framebuffer_selftest.sh`'s own
  synthetic `mouse_move` injection) restored a STALE `saved_pixels`
  buffer at the sprite's OLD logical position, and drew the "new"
  sprite at a position likewise miscalculated relative to where the
  visible sprite actually was after the scroll — producing a real,
  visible ghost trail (the screendump showed BOTH the old and new
  cursor positions lit simultaneously, double the expected pixel
  count). Root-caused by directly reading `cursor.c` and
  `fbconsole.c`'s scroll call site side by side, not guessed. Fixed by
  giving `cursor.c` two new public functions, `cursor_hide()`/
  `cursor_show()` (erase-before/redraw-after, the standard "hide the
  cursor around a bulk video operation" pattern real mouse drivers
  use), and wrapping `fbconsole.c`'s own `fb_scroll_up()` call with
  them — a small, deliberate new coupling (`fbconsole.c` now includes
  `cursor.h`), the same kind of cross-module coordination Milestone
  23's own ADR already established as sometimes necessary (its
  "two-consumers-one-queue mouse bug"). Re-verified with
  `test_framebuffer_selftest.sh` passing again, cursor at the exact
  expected position with no ghost, after the fix.
- `-d int,cpu_reset` trace: unchanged from Milestone 27 (1 `#BP`, 3
  `#PF`, zero kernel-caused resets).
- All twenty-five other smoke tests and all four host test suites
  re-verified passing on a clean rebuild after all fixes. Reap count
  raised 9 → 10 (client A + client B replacing Milestone 27's single
  client) — `test_exec_selftest.sh`/`test_fork_wait_selftest.sh`/
  `test_process_lifecycle_selftest.sh`/`test_ipc_shm_selftest.sh` all
  needed this same assertion updated. Booted 3 additional repeat times
  after all fixes — identical marker sequence, exact reap count (10),
  and exact present count (2) every time. Two of twenty-six tests
  (`test_blocking_wait_selftest.sh`, `test_boot_serial.sh`) showed a
  transient `os.iso: No such file or directory`/timeout failure during
  one rapid full-suite batch run; both passed cleanly re-run in
  isolation immediately after, with no stale `qemu-system-x86_64`
  process found (`pgrep` confirmed) — consistent with this project's
  own documented rapid-sequential-QEMU-boot flakiness precedent
  (Milestone 21/26 sessions), not a real regression.

## Known limitations (accepted for this milestone only)
Exactly two clients, fixed cascade placement — no dynamic window list,
no move/close/raise, no real input-driven focus yet (`Desktop.md`'s own
next arc item). **Windows are NOT immune to later console scroll
events, unlike the mouse cursor (now fixed).** `fb_scroll_up()` shifts
the entire physical framebuffer uniformly; this milestone fixed the
cursor's own resulting ghost-trail bug (`cursor_hide()`/`cursor_show()`
around the scroll), but a display-server window is owned by a separate
RING-3 PROCESS with no way for `fbconsole.c` to ask it to hide/redraw
itself around a scroll — so any console text printed AFTER a window is
presented will visually drag that window upward along with everything
else, exactly as this milestone's own smoke test discovered (and now
checks for, via relative geometry, rather than hiding the issue). This
is a real, growing architectural gap: the text console and the
window/cursor compositing layer currently share one physical
framebuffer with no separate surface and no "please redraw yourself"
protocol for a ring-3 window owner. Worth a dedicated fix (e.g.
reserving a fixed console region that can't encroach on the desktop
area, or a redraw-request IPC message to the display server) before or
during `Desktop.md`'s window-chrome/interactivity milestones, where a
visibly drifting window would actually be user-facing, not just a test
inconvenience.
