# ADR 0031: window chrome and basic widgets (title bars, drag-to-move, close button)

## Status
Accepted and verified — `make run` boots the real ISO; both demo
windows now have a real, server-drawn title bar, can be dragged to an
arbitrary position with the mouse, and can be closed via a real close
button — all proven with genuine QEMU-injected mouse input, not a
trusted self-report. This is the point Desktop.md's own milestone 6
describes as "where it starts feeling like a desktop rather than a
windowing demo" — after boot completes its `[OK]` self-checks, a real,
interactive multi-window desktop is already on screen. All twenty-seven
smoke tests and all four host test suites pass. `-d int,cpu_reset`
trace unchanged. Booted clean on the first real attempt for the core
feature; the two known follow-on adjustments (present-count self-test,
one pixel-math correction in an existing test) were anticipated and
fixed immediately from direct reasoning, not live failures.

## Context
Desktop.md's own milestone 6, next after Milestone 30 gave a click a
real, visible effect (raising a window). This milestone is what the
user explicitly asked for: boot through the `[OK]` checks, then have a
GUI that can actually be used.

## Decision

- **Chrome is entirely server-drawn, never client-drawn.** The title
  bar (a solid-color strip with a close-button square baked in) is a
  static buffer `display_server.c` owns and composites ABOVE a
  client's own canvas -- the client (`display_client_a.c`/`_b.c`)
  needed ZERO changes for this milestone. This is a deliberate
  architectural split, not just convenience: a client cannot fake or
  omit its own window decorations, the same reason real window
  managers draw chrome themselves rather than trusting the client to.
- **Three new input events, not a general "stream every mouse move"
  bus.** `INPUT_EVENT_DRAG` (`kernel/user/input_protocol.h`) is sent
  by `cursor.c` only when the cursor moves WHILE the button is already
  held -- never on a plain hover -- and `INPUT_EVENT_RELEASE` mirrors
  `INPUT_EVENT_CLICK`'s own press-edge design for the button-up
  transition. `kernel/drivers/input_router.c`'s own `notify_click()`
  became a generic `notify(event, x, y)` reused for all three, rather
  than three near-duplicate functions.
- **Per-event edge detection stays per-packet, not "state before vs.
  after the whole poll batch."** `cursor_poll()` already drains
  several queued mouse reports per call; scanning each one
  individually (as it already did for clicks) means a press-then-
   release that both happen to land in the same poll are still both
  caught, instead of looking like nothing changed net.
- **Drag state (which window, and the fixed offset from its own (x,y)
  to the point actually clicked) lives entirely in the server**, not
  echoed back by the client -- the server already owns geometry, and
  the client doesn't even know chrome exists.
- **Closing a window doesn't reclaim its resources** (no client
  teardown protocol exists) -- it's marked `closed` and skipped by
  hit-testing/compositing, with its old on-screen footprint
  (title + canvas) explicitly cleared to black and everything still
  open recomposited on top, reusing the exact same "opaque windows,
  paint bottom-to-top" reasoning Milestone 28 already established (so
  a still-open window that was partially covered by the one that just
  closed is correctly redrawn, not left with a hole). A REAL client
  exit/cleanup protocol is future work — see Known limitations.
- **`WINDOWS_TOTAL` stays fixed at 2** — hit-testing, raising, and the
  chrome/clear buffer sizes all still assume exactly two, fixed-size
  (200x150 max) windows, the same scope this whole demo has had since
  Milestone 27. A general N-window manager is future work, not this
  milestone's job (CLAUDE.md: don't build machinery beyond what's
  actually needed).

## Rejected alternatives
- **Client-drawn chrome** (each client renders its own title bar into
  a taller canvas). Rejected — see Decision; couples chrome to every
  client's own code and lets a buggy/hostile client fake or omit its
  decorations.
- **A general mouse-move event stream**, always sent regardless of
  button state. Rejected — nothing this milestone needs cares about a
  plain hover; `INPUT_EVENT_DRAG`'s own "only while held and moving"
  scope is exactly what dragging needs, no more.
- **Reclaiming a closed window's shm/process resources immediately.**
  Rejected for this milestone — no client-facing "please exit" protocol
  exists yet, and building one wasn't needed to prove drag/close work
  visually. Flagged explicitly as a known gap, not silently ignored.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-30
  marker unchanged, plus the server's own chrome/drag/close markers
  (only when a test or a real user actually triggers them).
  `kernel_main`'s own `sys_fb_present` count self-test updated from
  `!= 2` to `!= 4` (one title-bar present plus one canvas present per
  window, during the deterministic initial setup every boot performs).
- **`tests/qemu/test_display_server_selftest.sh` (pixel math updated,
  not its own structure)**: the FIRST screendump's expected teal pixel
  count changed from 15000 to 12000 -- a REAL, correctly-reasoned
  consequence of chrome, not a bug: client B's own title bar (drawn
  after client A, per initial z-order) now ALSO covers part of client
  A's exposed canvas, beyond what client B's canvas alone already
  did. Verified by hand-deriving the exact expected overlap geometry
  (150px × 120px = 18000px covered of A's 30000px canvas = 12000
  visible) BEFORE changing the assertion, then confirming the actual
  screendump matched precisely -- not loosened into a tolerance.
- **`tests/qemu/test_window_chrome_selftest.sh` (new)**: injects a real
  press on client B's title bar (clear of its close button), a real
  450px drag, and a real release, then confirms via screendump that
  client B's canvas is EXACTLY at its new, expected position (a full,
  unbroken rectangle, disjoint from client A entirely, so the check is
  unambiguous). Then injects a real click on client A's own close
  button and confirms, via a second screendump, that client A's canvas
  has vanished COMPLETELY (not shrunk, not moved -- absent), while
  client B's (dragged) canvas remains the exact full rectangle,
  unaffected.
- `-d int,cpu_reset` trace: unchanged (1 `#BP`, 3 `#PF`, zero
  kernel-caused resets).
- All twenty-six other smoke tests and all four host test suites
  re-verified passing on a clean rebuild. Booted 3 additional repeat
  times — identical marker sequence, exact reap count (9), and exact
  present count (4, for a plain boot with no input injected) every
  time.

## Known limitations (accepted for this milestone only)
Still exactly two fixed-size (200x150 max), fixed-identity windows —
no new windows can ever be created, no dynamic window list. Closing a
window doesn't reclaim its client process, shared-memory buffer, or
server-side slot — the client (`display_client_a.c`/`_b.c`) has
already exited long before a close is even possible (it presents once
during boot setup and returns), so the "leak" is bounded and one-time,
not growing, but a REAL close protocol (telling a still-running client
to exit, and the server actually freeing its own `shm_map()`
reference) is real future work once windows host actual long-running
applications (Desktop.md's own next arc item) rather than one-shot
demos. No minimize, no resize, no keyboard focus routing to whichever
window is "active." The close button's own visual (a solid magenta
square) is a placeholder, not a real "X" glyph — cosmetic only, not a
correctness gap.
