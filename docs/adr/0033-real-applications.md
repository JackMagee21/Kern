# ADR 0033: real applications (a genuinely persistent, self-redrawing window)

## Status
Accepted and verified — `make run` boots the real ISO; a third window
(kernel/user/pulse_app.c) now sits alongside client A and client B,
proving something no earlier client ever had to: a real application can
stay running indefinitely and keep updating its own on-screen content,
not just present once and exit. This is Desktop.md's own final GUI-arc
item — "a small number of genuinely different programs... something
interactive enough to prove input routing works end to end" — completed
from the complementary angle Milestone 29-31 already proved (real click/
drag/close routing): a real, live *application*, not just real *input*.
All twenty-eight smoke tests (twenty-seven pre-existing, one new) and
all four host test suites pass. Booted 3 additional clean repeats under
real KVM acceleration (ADR 0032) with zero `#GP` faults.

## Context
Desktop.md's GUI arc had, by Milestone 31, a real multi-window desktop
with drag/close/raise, but every client so far (`display_client_a.c`,
`display_client_b.c`) followed the same shape: request a canvas, fill
it once, present it once, exit. Nothing in this codebase had ever
proven a window could change what it shows AFTER landing on screen, or
that a client process could run forever the way `display_server.c`
itself already does (Milestone 30). This milestone closes that gap with
the smallest thing that proves it end to end: a window that cycles
through a small fixed color palette forever, pacing itself with a plain
`sys_nop` spin (the same bounded, deterministic pattern
`kernel/user/fork_demo.asm` already established for its own loop).

## Decision

- **One new protocol message, not a new subsystem.**
  `DISPLAY_OP_REDRAW` (`kernel/user/display_protocol.h`, opcode 6) is a
  no-fields ping: "I already wrote fresh pixels into the SAME
  shared-memory buffer you already have mapped -- recomposite
  everything." The server's handler is one line
  (`case DISPLAY_OP_REDRAW: composite_all(); break;`) because
  `composite_all()` already re-reads every window's stored `va` from
  scratch on every call -- no new per-window server-side state needed
  at all, the same "opaque windows, painted bottom-to-top" reasoning
  Milestone 28 already established.
- **The client never re-maps its shm.** `sys_shm_map()` is called
  exactly once, at startup, same as A/B -- only the CONTENTS at that
  already-mapped address change between frames. Re-mapping repeatedly
  would burn through a process's fixed 1MiB shm VA budget for no
  reason; writing new bytes into memory it already owns needs nothing
  new from the kernel at all.
- **A third window, not a repurposed A/B.** `display_client_a.c`/`_b.c`
  are load-bearing for exact-pixel assertions in
  `test_display_server_selftest.sh`/`test_window_chrome_selftest.sh` --
  changing either risked breaking tests that have nothing to do with
  this milestone. `kernel/user/pulse_app.c` is a genuinely new third
  client, placed at `(650, 520)` -- far enough from A/B's own
  `(100-349, 500-699)` footprint that it can never overlap either,
  keeping every existing pixel assertion untouched by construction, not
  by accident.
- **`raise_to_top()` generalized from a hardcoded 2-element swap to a
  real shift loop.** `WINDOWS_TOTAL` moving from 2 to 3 exposed that the
  old swap-positions-0-and-1 logic was only ever correct for exactly
  two windows -- raising a window out of the bottom of a 3-deep stack
  needs an actual shift, not a swap. The general version reduces to the
  identical swap whenever `WINDOWS_TOTAL == 2`, so this is a correctness
  fix for N > 2, not a behavior change for the existing N = 2 case.
- **Joins the SAME go-signal chain, one link further.** Client A signals
  B (Milestone 28); B now also signals the pulse app once its OWN
  window has landed, extending the deterministic A -> B -> C ordering
  Milestone 28 established rather than inventing a parallel mechanism.
  This is what makes the server's initial setup loop's fixed
  `window_x[2]/window_y[2]` assignment safe -- the pulse app's own
  `DISPLAY_OP_REQUEST` is guaranteed to be the THIRD one the server
  ever receives, by construction, not by scheduling luck.
- **Created in the "permanent process" zone, like the server itself.**
  `pulse_app_process` is created in `kernel.c` BEFORE the frame-leak
  baseline (`frames_before_processes`), alongside `display_server_process`
  and the Milestone 6/25 permanent kernel threads -- this process's own
  event loop never returns during a normal boot, so if it were created
  after the baseline like a bounded demo, the reap-count self-test gate
  every other test depends on would hang forever.
- **`sys_fb_present` count self-test changed from exact equality to a
  floor (`< 6`).** Every earlier milestone could assert an EXACT present
  count because every window presented exactly once during boot's
  deterministic setup window. That's no longer true: the pulse app keeps
  redrawing (and thus re-triggering `composite_all()`, 6 more presents
  per redraw) in the background, on a schedule this self-test has no way
  to pin down exactly. `< 6` stays exactly as strict at catching the
  real failure this check exists for (some window's chrome or canvas
  silently never landing during initial setup) while correctly
  tolerating any number of legitimate extra redraws that happen to have
  already occurred by the time the check runs.
- **`DISPLAY_SERVER_PERMANENT_CANVAS_PAGES` raised from 60 to 75, for a
  structurally NEW reason, not just a bigger version of the old one.**
  A/B's 60-page deficit exists because the SERVER's second reference to
  their canvases never drops (the server never exits) even though A/B
  themselves DO exit and drop their own first reference. The pulse
  app's additional 15 pages (`150 * 100 * 4` bytes, ceil-divided by
  `PMM_FRAME_SIZE`) never come back for a different reason: the pulse
  app's OWN first reference never drops either, since IT never exits.
  Derived by hand from `shm.c`'s own `ceil(size / PMM_FRAME_SIZE)`
  page-rounding logic, then confirmed by an actual clean boot showing
  no leak-check panic -- not assumed safe in advance.
- **Palette deliberately avoids anything resembling the cursor's red.**
  An early version of this palette included a near-red entry
  (`0x00FF3333`) that turned out to collide with
  `tests/qemu/test_framebuffer_selftest.sh`'s existing whole-screen
  cursor-color scan (`r>200, g<60, b<60`, written back when only an 8x8
  sprite could ever be that shade) -- once the pulse app's 150x100
  window could ALSO be that shade, the same scan merged both into one
  bounding box and broke that test's exact-position assertion. Found by
  actually running the full regression suite, not assumed safe in
  advance (CLAUDE.md: "actually run the... test and show its output").
  Fixed by replacing that entry with purple (`0x00CC33FF`), checked
  directly against every color matcher in every `tests/qemu/*.sh` file
  to confirm it collides with none of them, not just guessed distinct.

## Rejected alternatives
- **A real wall-clock-driven animation** (reading actual time via
  `kernel/drivers/rtc.c`'s `rtc_read()`). Rejected for this milestone --
  no `sys_*` syscall wrapper exists for it yet (`rtc_read()` is
  kernel-only), and a real clock app is reasonable, clearly-scoped
  future work once one does; a `sys_nop`-paced palette cycle proves the
  same "genuinely live, self-redrawing window" property without that
  new dependency.
- **Text/font rendering for a more legible demo app** (e.g. a real
  digital clock face). Rejected as a second new subsystem this
  milestone doesn't need -- a solid-color palette proves persistence and
  redraw just as rigorously, with zero new rendering machinery.
- **Repurposing client A or B into the persistent app.** Rejected --
  both are load-bearing for other tests' exact-pixel assertions; a
  genuinely new, spatially disjoint third client keeps every existing
  test's geometry assumptions valid without special-casing.
- **A general N-window architecture (dynamic window list, arbitrary
  client count).** Rejected as beyond this milestone's actual need --
  `WINDOWS_TOTAL` is still a fixed compile-time constant (now 3, not a
  general count), matching CLAUDE.md's "don't build machinery you don't
  need" stance; `raise_to_top()`'s generalization was needed correctness
  for THIS fixed N, not a step toward arbitrary N.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-32
  marker unchanged, plus the pulse app's own creation/present/animating
  markers, plus a third `[OK] display server: presented window 0x2`
  line during initial setup.
- **`tests/qemu/test_pulse_app_selftest.sh` (new)**: boots headless,
  waits for the shell prompt, then POLLS repeated real screendumps
  (there is no serial marker for "a redraw happened" -- the server's
  `DISPLAY_OP_REDRAW` handler deliberately logs nothing, matching
  `composite_all()`'s own "no new bookkeeping" design) sampling a fixed
  point inside the pulse app's own canvas, classifying the color against
  its known palette, and confirming the sampled color genuinely CHANGES
  within a bounded real-time window -- proof the window is actually
  live, not just correctly presented once. Passed, observed changing
  from palette index 3 to index 0 in one representative run.
- All twenty-seven pre-existing smoke tests re-verified passing on a
  clean rebuild, including `test_display_server_selftest.sh` and
  `test_window_chrome_selftest.sh`'s own exact-pixel assertions for
  clients A/B, unaffected by construction (the pulse app never overlaps
  either). All four host test suites re-verified passing (unaffected --
  no `libk/` changes this milestone).
- Booted 3 additional repeat times under real KVM acceleration (ADR
  0032) -- identical marker sequence each time, zero `#GP` faults,
  reached the shell every time.
- Caught and fixed one real regression before considering this
  milestone done (see Decision's last bullet): the pulse app's original
  palette collided with an existing test's cursor-color matcher, found
  by actually running the full suite, not by inspection.

## Known limitations (accepted for this milestone only)
Still a fixed, compile-time `WINDOWS_TOTAL = 3` -- no dynamic window
creation, no way to launch a new instance of the pulse app or any other
program from the running shell. The pulse app never responds to input
(no click/drag/close handling of its own -- it's a pure output demo,
proving redraw, not interaction, which Milestone 29-31 already proved
for A/B). Its "animation" is a plain solid-color palette cycle, not real
content -- a real clock, or any app with genuine state, is future work.
No client exit/cleanup protocol still exists (Milestone 31's own known
limitation, unchanged) -- moot here specifically since the pulse app, by
design, never exits during a normal boot.
