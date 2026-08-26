# ADR 0035: a real clock app

## Status
Accepted and verified — a fourth window, `kernel/user/clock_app.c`,
renders real `HH:MM:SS` wall-clock time (read via a new `sys_rtc_read`
syscall) using a small embedded digit font, and only redraws when the
displayed second genuinely changes. Booted clean on the first real
attempt. A real screendump comparison, three seconds apart, shows the
rendered digits actually change — not a static image. Supports the
same real close/exit protocol (ADR 0034) from the moment it was
created, not retrofitted later. All twenty-nine pre-existing smoke
tests plus this milestone's own new one (thirty total) pass; booted
clean under real KVM through the exact close/exit/reap sequence, zero
`#GP` faults.

## Context
Milestone 33's own Rejected alternatives explicitly named this idea
and explicitly deferred it: "a real clock (reading actual wall-clock
time) is reasonable future work once a time-reading syscall exists
(`kernel/drivers/rtc.c`'s `rtc_read()` is kernel-only today, no `sys_*`
wrapper yet)." `future.md`'s own "Reasonable next steps" repeated the
same framing, and Milestone 34 (`sys_ipc_try_recv`) removed the OTHER
blocker: a persistent client that also needs to notice a close request
without giving up its own loop's pacing. With both prerequisites now
in place, this milestone builds the actual clock.

## Decision

- **One new syscall, `sys_rtc_read` (`SYS_RTC_READ`, number 14) — a
  thin wrapper around an EXISTING kernel primitive, not a new
  subsystem.** `kernel/drivers/rtc.c`'s `rtc_read()` already existed,
  fully correct and already proven by `test_rtc_selftest.sh` and the
  `date` shell command; it was kernel-only purely because CMOS port
  I/O (`in`/`out`) is a ring-0-only instruction this kernel never
  grants ring 3 an IOPL for (CLAUDE.md: hardware registers only via
  explicit port helpers, never opened to userspace directly). The
  syscall validates the caller's out-pointer (same pattern as every
  other out-pointer syscall here) and calls `rtc_read()` directly — no
  new logic, no new hardware access path.
- **`rtc_time_t` (`kernel/drivers/rtc.h`) is shared with userspace
  directly, not duplicated.** It's already a plain POD struct with only
  `<stdint.h>` fields, no kernel-only dependencies — safe to include
  from `kernel/user/rt/syscall.h` the same way `ipc_message_t`
  (`kernel/ipc/ipc_message.h`) already is. Duplicating an identical
  struct definition just to keep a directory boundary would be pure
  ceremony for zero actual benefit.
- **A small, self-contained 3x5 digit font (11 glyphs: 0-9 and `:`),
  hardcoded directly in `clock_app.c` — deliberately NOT a dependency
  on `kernel/drivers/fbconsole.c`'s own font/console machinery.**
  That font is tied to the kernel's own text console (rows/columns,
  scrolling, a fixed glyph set for the WHOLE character range) — not a
  fit for one client drawing eight fixed glyph slots into a buffer it
  already owns. Milestone 33's own Rejected alternatives already
  reasoned through why pulling in real text rendering as a shared
  subsystem wasn't warranted; eleven small bitmaps is a bounded,
  self-contained addition, not that subsystem — CLAUDE.md's "don't
  build machinery you don't need" cuts the same way here.
- **Only redraws when the displayed SECOND actually changes**, not on
  every poll. `FRAME_DELAY_NOPS` is deliberately smaller than the pulse
  app's own (50000 vs. 200000) so the poll itself is more frequent —
  but polling `sys_rtc_read()` and comparing costs far less than a full
  canvas re-render plus a server round-trip, so polling more often
  while REDRAWING only on a real change is strictly better than either
  polling rarely (feels laggy) or redrawing every poll (wastes cycles
  recompositing pixels that are already correct on screen).
- **Joins the go-signal chain one link further: A -> B -> pulse app ->
  clock app.** The pulse app's own boot message now carries the clock
  app's pid as `fields[1]` (mirroring exactly how client B's own boot
  message already carries the pulse app's pid); the pulse app forwards
  `DISPLAY_OP_GO` to the clock app once its OWN canvas is confirmed on
  screen. Keeps the server's fixed `window_x[3]`/`window_y[3]`
  assignment deterministic by construction, the same reasoning every
  earlier link in this chain already established (Milestone 28 onward).
- **Supports `DISPLAY_OP_EXIT` from day one, not retrofitted.** Unlike
  the pulse app (which predates Milestone 34 and needed a follow-up
  milestone to gain this), the clock app polls for it with
  `sys_ipc_try_recv` in the exact same place/shape from its very first
  version — proof the Milestone 34 mechanism generalizes cleanly to a
  second persistent client, not just the one it was built for.
- **`WINDOWS_TOTAL` raised 3 -> 4 needed no other `display_server.c`
  change.** `raise_to_top()`'s general shift loop (Milestone 33) and
  `composite_all()`'s own "read every window's current `va`" design
  already generalize to any `WINDOWS_TOTAL` — only the window count and
  the new window's `window_x[3]`/`window_y[3]` placement needed adding.
- **Frame-leak accounting extended by the same, now-established
  pattern, not a new one.** The clock app's own canvas
  (190 * 50 * 4 = 38000 bytes, `ceil(38000 / 4096)` = 10 pages) is a
  permanent deficit for the identical reason the pulse app's 15-page
  deficit already is: it's a persistent client (created before
  `kernel_main`'s own frame-leak baseline) whose own first shm
  reference is never dropped during a normal boot.
  `DISPLAY_SERVER_PERMANENT_CANVAS_PAGES` raised 75 -> 85 (60 + 15 +
  10), each term independently re-derived from the relevant source's
  own dimensions, not just incremented by feel.

## Rejected alternatives
- **An analog clock face** (a rotating "hand" drawn via a lookup table
  of endpoint coordinates, avoiding both floating point and a font).
  Rejected — a digital readout is more legible with less code (a fixed
  small font vs. a full 60-entry coordinate table plus line-drawing),
  and CLAUDE.md's "no FP/SSE until FPU context-switch milestone
  exists" rules out a trig-based approach entirely regardless.
- **Reusing `fbconsole.c`'s own embedded font.** Rejected — see
  Decision; that font and its rendering path are console-shaped
  (rows/columns/scrolling), not a fit for a client drawing into its
  own arbitrary shm buffer, and pulling it in as a shared dependency
  would be new cross-cutting machinery for a problem eleven small
  hardcoded glyphs already solve without it.
- **Redrawing on a fixed timer tick regardless of whether the second
  changed.** Rejected — would waste a full canvas fill + server
  round-trip on every poll for no visible benefit; comparing
  `now.second != last_time.second` is a cheap integer check that keeps
  redraws meaningful.
- **Packing `rtc_time_t`'s fields into `rax`/a single register instead
  of an out-pointer.** Rejected — every other multi-field syscall
  result in this codebase (`sys_wait`'s exit code, `sys_ipc_recv`'s
  message) already uses an out-pointer; bit-packing six fields would
  save nothing here and break with the codebase's own established
  convention for "return more than one thing."

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-34
  marker unchanged, plus the clock app's own creation/present/ticking
  markers, plus a fourth `[OK] display server: presented window 0x3`
  line during initial setup. Booted clean on the FIRST real attempt.
- Manual verification during development: two real QEMU screendumps,
  2.5 real seconds apart, sampled the clock window's own region —
  2016/1872 digit-colored pixels present in each (proving real glyphs
  rendered, not a blank canvas), and 144 pixels genuinely differed
  between the two (proving the displayed time actually changed).
- **`tests/qemu/test_clock_app_selftest.sh` (new)**: two screendumps
  three real seconds apart confirm the clock window's region contains
  both the digit color and the background color (real glyphs actually
  drew) AND that the region is NOT byte-for-byte identical between the
  two (real ticking, not a static image) — the same "observe a real
  state change, don't infer from a guessed delay" discipline
  `test_pulse_app_selftest.sh` already established. Then injects a
  real click on the clock's own close button and confirms the SAME
  three independent facts `test_window_close_exit_selftest.sh`
  (Milestone 34) already established for the pulse app: the server's
  close marker, the client's own exit-received marker, and a real reap
  marker after the shell prompt — proving `DISPLAY_OP_EXIT` generalizes
  to a second persistent client cleanly. A final screendump confirms no
  digit-color pixels remain.
- Booted clean under real KVM acceleration with the identical
  close-click sequence: `[OK] display server: closed window 0x3` ->
  `[OK] clock app: received exit request, exiting` -> a real reap
  marker, zero `#GP` faults.
- All twenty-nine pre-existing smoke tests and all four host test
  suites re-verified passing on a clean rebuild (unaffected — the new
  window is spatially disjoint from every existing window and the
  console's own reserved scroll region by construction).

## Known limitations (accepted for this milestone only)
Still assumes the CMOS RTC's own century convention (`rtc.c`'s existing
`2000 + year` assumption, unchanged) and displays time only to
one-second resolution (the RTC's own native granularity — no
sub-second timing was ever in scope). No timezone handling — displays
whatever the RTC itself is configured to (raw hardware time, exactly
what `rtc_read()`'s existing contract already promised, unchanged by
this milestone). No date display (day/month/year) — a real clock,
deliberately not a calendar; adding one would be a small, well-scoped
future extension of the same font if ever wanted. `WINDOWS_TOTAL` is
still a fixed compile-time constant (now 4) — dynamic window creation
remains its own separate, un-started `future.md` candidate.
