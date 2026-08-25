# ADR 0030: real click-driven window raising, and scroll-immune windows

## Status
Accepted and verified — `make run` boots the real ISO; the display
server is now genuinely persistent, subscribes to real hardware input
(Milestone 29's mechanism), and a real QEMU-monitor-injected PS/2
click on client A's own exclusive region visibly raises it above
client B — proven by a SECOND screendump showing the overlap region
flip from orange to teal, pixel-for-pixel, not a trusted self-report.
Along the way, this milestone also delivered the real fix ADR 0028
flagged but deferred (windows drifting from console scroll), and found
and fixed two genuine, pre-existing concurrency/correctness bugs in the
framebuffer console and cursor driver — neither planned in advance,
both caught via direct screendump/serial evidence, not guessed. All
twenty-six smoke tests and all four host test suites pass. `-d
int,cpu_reset` trace unchanged. Booted clean for the core display-server
redesign; the console/cursor bugs were found and fixed via careful,
methodical diagnosis (documented in full below) before the milestone
was considered done.

## Context
Desktop.md's own milestone 6 ("window chrome and basic widgets") was
expanded, once Milestone 29 shipped, to also absorb ACTUALLY WIRING
that milestone's click-delivery mechanism into `display_server.c` —
Milestone 29 deliberately proved delivery in isolation with a
throwaway demo, explicitly deferring "does a click do anything visible"
to this point (ADR 0029's own Decision). Making a window raise in
response to a real click needs `display_server.c`'s own lifecycle
redesigned from "serve N clients then exit" (Milestones 27/28) into a
persistent event loop — genuinely different work from anything those
two milestones did.

## Decision

- **The display server becomes persistent, retiring Milestone 29's
  standalone demo.** `input_focus_demo.c` was explicitly built as an
  isolated proof that hardware clicks could reach A ring-3 process at
  all (ADR 0029's own Decision: "prove the mechanism in isolation, NOT
  wire it to display_server.c yet"). Now that `display_server.c` is the
  REAL, meaningful subscriber, keeping the standalone demo around would
  create an unresolvable conflict: `sys_input_subscribe()`'s own
  kernel-enforced exclusivity (ADR 0029) means only ONE of the two
  could ever actually win the subscription each boot. Retired
  (`input_focus_demo.c`, its blob, and its own dedicated test all
  deleted) rather than kept as now-permanently-losing dead weight — the
  same "delete what's genuinely superseded" precedent Milestone 28
  already set for the original single-client `display_client.c`.
- **The server is created BEFORE `kernel_main`'s frame-leak baseline,
  exactly like the demo it replaces was.** It never exits during a
  normal boot (same "waits forever for input, not a hang" reasoning
  ADR 0029 already established) — if it stayed in the reap-count gate's
  target, every other headless test would hang forever. This LOWERS
  the reap-count target for the first time ever (10 → 9: the server's
  own two clients still reap normally, but the server itself no longer
  does) — see `kernel_main`'s own updated comment.
- **A real, EXPECTED (not leaked) deficit from the frame-leak
  baseline now exists, found on the very first boot after this
  redesign.** The persistent server holds a SECOND, permanent
  reference to each client's shm canvas (needed to recomposite on a
  raise) that a normal, bounded process would have dropped on exit.
  Verified precisely (not hand-waved): captured the actual before/after
  `pmm_frames_free()` values via a temporary debug print, confirmed the
  deficit was EXACTLY 60 pages (2 windows × ceil(200×150×4 /
  4096) = 30 pages each), then hardcoded that exact, derived expectation
  (`DISPLAY_SERVER_PERMANENT_CANVAS_PAGES`) rather than loosening the
  check into a tolerance — any OTHER delta still panics, so this stays
  exactly as strict at catching a real leak as before.
- **A real hit-test + raise, not a general compositor.** `hit_test()`
  scans z-order top-down (real window-manager semantics: whichever
  window is drawn LAST wins any point both cover) and returns which
  z-order POSITION was hit. Raising is a plain swap (`z_order[0]` ↔
  `z_order[1]`), not a general shift loop — `WINDOWS_TOTAL` is fixed at
  2, and a loop that never runs more than one shape is just unused
  complexity (CLAUDE.md: don't build machinery you don't need).
  Recompositing after a raise reuses the EXACT same "opaque windows,
  painted bottom-to-top" reasoning the initial setup already relies on
  (ADR 0028) — no new compositing logic at all, just a new call to the
  same `composite_all()` the initial loop already effectively was.
- **Windows relocated to y ≥ 480 — the REAL fix for ADR 0028's flagged
  drift, not another workaround.** ADR 0028 explicitly flagged windows
  as vulnerable to `fb_scroll_up()`'s full-framebuffer shift and
  deferred fixing it. This milestone made it a genuine FUNCTIONAL bug,
  not just cosmetic: hit-testing uses windows' NOMINAL coordinates,
  but a click's reported position comes from the cursor's REAL on-screen
  location — if a window had visually drifted from where the server
  thinks it is, a click on its real position would miss its nominal
  rect entirely. Fixed at the root: `fb_scroll_up()` gained a
  `region_height` parameter (`kernel/drivers/framebuffer.c`/`.h`) so a
  scroll can be bounded to LESS than the full screen;
  `kernel/drivers/fbconsole.c` now reserves a fixed
  `FBCONSOLE_MAX_HEIGHT_PX` (480) for its own text/scroll region, and
  `display_server.c`'s windows moved to y ≥ 480 (documented, duplicated
  constant on both sides — the same "plain integers, not a struct
  layout" pattern already used for syscall numbers, ADR 0024) —
  permanently outside the scroll's reach, not just less likely to be
  hit by it.
- **Two more real bugs, found via direct screendump/serial evidence
  while verifying the above, neither planned in advance:**
  1. `fbconsole.c`'s `draw_glyph()` writes directly via `fb_put_pixel()`
     with no cursor awareness at all — any character overlapping the
     cursor's own 8×8 footprint corrupted it. Fixed by wrapping
     `fbconsole_putc()`'s entire body in `cursor_hide()`/`cursor_show()`
     (same shape as the scroll/clear wraps ADR 0028 already
     established).
  2. **The real root cause of an actual, screendump-visible
     corruption** (a scattered red "ghost" pattern across a wide y
     range, `saved_pixels` literally captured as solid cursor-red 308
     times in one boot, found via direct instrumentation, not
     guessed): `console_putc()` — reachable from EVERY ring-3 process's
     `sys_write()`, with interrupts enabled and no synchronization at
     all — shares `cursor.c`'s `cursor_visible`/`saved_pixels` state
     machine across the WHOLE system with no mutual exclusion.
     Milestones 27-29 added enough ring-3 processes printing boot
     markers that the preemptive scheduler's own timer IRQ could
     genuinely interleave two DIFFERENT processes' calls to this
     function mid-character — each individually correct in isolation,
     never mutually exclusive. Fixed by disabling interrupts for
     `fbconsole_putc()`'s (and `fbconsole_clear()`'s) entire body, using
     the SAME save/restore-flags idiom `scheduler_wake()` already
     established (Milestone 25) rather than a bare `cli`/`sti` (which
     would incorrectly force interrupts ON even when called from code
     that had them deliberately off, e.g. every early-boot print before
     `kernel_main`'s own `sti`) — CLAUDE.md: "disable interrupts around
     a critical section... keep it as short as provable," bounded here
     to one glyph draw plus at most one bounded-region scroll, never
     unbounded. `serial_putc()` (`console_putc()`'s OTHER job) is
     untouched by this race and stays outside the disabled window,
     keeping it as small as the actual shared state requires.

## Rejected alternatives
- **Keeping `input_focus_demo.c` alongside the real server.** Rejected
  — an unresolvable subscription-exclusivity conflict, see Decision.
- **A general N-window raise (shift loop) instead of a 2-element
  swap.** Rejected — `WINDOWS_TOTAL` is fixed at 2 for this milestone;
  a loop that never exercises more than one case is unused complexity.
- **Loosening the frame-leak self-test into a tolerance range** instead
  of an exact, derived expected deficit. Rejected — an exact,
  understood number is exactly as strict as before; a loose tolerance
  would have silently hidden a REAL future leak of a similar size.
- **A smaller console scroll region as a smaller window for the same
  drift bug**, instead of making windows genuinely immune. Rejected —
  ADR 0028 already tried the "smaller window for the bug" framing
  implicitly by not fixing it at all; this milestone's own hit-testing
  need made clear that only a structural fix (windows permanently
  outside the scrolled region) actually closes the gap.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-29
  marker unchanged, plus: the server's own `sys_input_subscribe`
  markers, both windows presented at their new y ≥ 480 positions, and
  (only when a test actually injects one) a routed-click log line plus
  `[OK] display server: raised window 0x0`.
- **`tests/qemu/test_display_server_selftest.sh` (substantially
  rewritten)**: after the FIRST screendump's existing bound/z-order
  checks pass (window Y coordinates updated for the relocation, the
  underlying relative-geometry technique from ADR 0028 unchanged), a
  REAL `mouse_move`/`mouse_button` (the same technique
  Milestones 16/23/the retired Milestone 29 test established) is
  injected onto client A's own exclusive region; after the server's own
  log line confirms the raise, the cursor is moved off-canvas (its own
  8×8 sprite would otherwise cover part of what's being measured — a
  real, understood interaction found and accounted for, not a bug) and
  a SECOND screendump is taken. That screendump must show the EXACT
  mirror image of the first: client A now the full 30000-pixel
  rectangle, client B reduced to the 15000-pixel L-shape — the overlap
  region genuinely changed owner.
- Reap count lowered 10 → 9 (`test_exec_selftest.sh`/
  `test_fork_wait_selftest.sh`/`test_process_lifecycle_selftest.sh`/
  `test_ipc_shm_selftest.sh` all updated, with `kernel_main`'s own
  process-lifecycle self-test now checking for the exact, derived
  60-page deficit rather than a bare equality).
- `-d int,cpu_reset` trace: unchanged (1 `#BP`, 3 `#PF`, zero
  kernel-caused resets) — none of this milestone's new code paths
  fault.
- All twenty-five other smoke tests and all four host test suites
  re-verified passing on a clean rebuild. Booted 3 additional repeat
  times — identical marker sequence, exact reap count (9), and exact
  present count (2, for a plain boot with no click injected) every
  time.

## Known limitations (accepted for this milestone only)
Still exactly two clients, fixed cascade placement — no dynamic window
list, no move/close, no window chrome/widgets yet (Desktop.md's own
next arc item). A THIRD client's `DISPLAY_OP_REQUEST` would currently
be silently ignored by the server's persistent loop (it only recognizes
`INPUT_EVENT_CLICK` once initial setup finishes) — fine for exactly 2,
not a general multi-window server yet. Raising only happens on a LEFT
click DOWN edge landing on a non-topmost window; there's still no
drag, no close, no focus-follows-click keyboard routing. The console's
own reserved height (`FBCONSOLE_MAX_HEIGHT_PX = 480`) is a fixed
constant, not derived from how much boot text actually exists — a
future boot sequence that prints enough text to need more than 60
visible rows just scrolls more within that bound (still correct, just
more scroll events), and a screen mode negotiated shorter than 480px
falls back to using the whole screen for console text (graceful
degradation, `fb_scroll_up()`'s own region_height clamp), which would
NOT leave room for windows below it — an accepted, unlikely edge case
this milestone doesn't handle specially.
