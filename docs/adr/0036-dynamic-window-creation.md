# ADR 0036: dynamic window creation

## Status
Accepted and verified — a real shell command (`spawn pulse` / `spawn
clock`) launches a genuinely NEW instance of an already-embedded GUI
program at runtime, requests and is granted a window from the ALREADY-
RUNNING display server, and renders on screen alongside the four
boot-time windows — the last concrete item `future.md`'s own
"Reasonable next steps" had flagged after Milestone 35. Booted clean.
New smoke test passes; all thirty pre-existing smoke tests and all
four host suites re-verified passing. Booted clean under real KVM
through the full spawn/spawn/close/exit/reap sequence, zero `#GP`
faults. One real, non-kernel finding surfaced and root-caused during
verification (see Decision's last bullet and Known limitations).

## Context
`future.md`'s "Reasonable next steps" (as of Milestone 35) named this
as the one remaining concrete GUI-arc-adjacent candidate: "spawning a
NEW window/program instance from the running shell, rather than every
window being a fixed, compile-time `WINDOWS_TOTAL` slot created once
at boot." Two things had to give: `display_server.c`'s own window
array was sized and indexed by a compile-time constant used
EVERYWHERE (composite/hit-test/raise), and every existing client
(A/B/pulse/clock) only ever requests a window as part of a strictly
sequential, deterministic go-signal chain the server's own boot-time
loop expects.

## Decision

- **A fixed-CAPACITY array, not a general dynamic list.** `windows[]`/
  `z_order[]` grew from `WINDOWS_TOTAL` (a single constant meaning both
  "how many at boot" and "the array size") to two separate things:
  `WINDOWS_BOOT_COUNT` (4, unchanged meaning) and `WINDOWS_CAPACITY`
  (8, the array's real size) plus a new runtime `windows_used` variable
  that starts at `WINDOWS_BOOT_COUNT` and grows by one per successful
  spawn. Same "bounded array, scanned/managed, sized generously"
  pattern `kernel/sched/scheduler.c`'s own `MAX_LIVE_TASKS` registry
  and `kernel/drivers/mouse.c`'s own event queues already established
  — not a new one invented here.
- **`composite_all()`/`hit_test()`/`raise_to_top()` needed exactly one
  change each: read `windows_used` instead of the old compile-time
  constant.** None of the three ever assumed anything else about the
  bound (Milestone 33 had already generalized `raise_to_top()`'s own
  shift logic to any count) — this is the SAME generalization Milestone
  35 already got for free when `WINDOWS_TOTAL` went 3 → 4, just now
  from a variable instead of another constant.
- **`handle_dynamic_request()`: the SAME REQUEST/GRANT/PRESENT/ACK
  handshake the boot-time loop already performs, reachable from the
  persistent event loop instead of only a dedicated setup loop.**
  Grants `(0, 0)` — an EXISTING failure shape every client's own
  handshake already checks for — if `WINDOWS_CAPACITY` is reached,
  needing no new client-side logic at all.
- **A real, found-in-review correctness fix: waiting for a specific
  client's `DISPLAY_OP_PRESENT` can't just call `sys_ipc_recv()` once
  and assume the result matches.** The boot-time loop's identical-
  looking wait is safe only because it runs before input is even
  enabled (`kernel.c`'s own ordering) — a dynamic spawn happens with
  input already flowing, so an unrelated message (a mouse click on an
  existing window) could genuinely arrive first and get misread as the
  new client's shm id. Fixed by factoring the persistent loop's own
  inline switch into a reusable `dispatch_message()`, which
  `handle_dynamic_request()`'s own wait loop now calls on anything that
  ISN'T the expected `PRESENT`, then keeps waiting — process everything
  else normally, don't block the world for one pending reply.
- **A second real bug, ALSO found in review before ever booting:
  `INPUT_EVENT_CLICK`/`DRAG`/`RELEASE` (`input_protocol.h`: 1/2/3) and
  `DISPLAY_OP_REQUEST`/`GRANT`/`PRESENT` (`display_protocol.h`: 1/2/3)
  are two independently-numbered protocols that happen to share the
  exact same low opcode values.** Never a problem before this
  milestone (`DISPLAY_OP_REQUEST` was only ever consumed by the
  dedicated boot-time loop, never `dispatch_message()`'s own switch) —
  became a real collision the moment both needed to be dispatchable
  from the SAME switch. Fixed using an existing, verified invariant
  (read directly from `input_router.c` and `ipc_message.h`, not
  assumed): a kernel-originated `INPUT_EVENT_*` message's `sender_pid`
  is always 0 (`input_router_notify()` calls `ipc_send()` directly,
  never `sys_ipc_send()`, so the struct-literal default stands), and no
  real ring-3 client can ever legitimately have pid 0 — so
  `dispatch_message()` now branches on `sender_pid == 0` FIRST, then on
  the opcode within each branch.
- **`kernel/shell.c` gained a `spawn` command that injects a boot
  message PLUS a synthetic `DISPLAY_OP_GO`, rather than restructuring
  `pulse_app.c`/`clock_app.c` to make their go-signal wait optional.**
  Both programs already unconditionally wait for a go-signal before
  their first `DISPLAY_OP_REQUEST` (the existing Milestone 33/35
  chain); satisfying that SAME wait from the kernel lets a dynamically
  spawned instance reuse both programs completely unmodified. This is
  the one deliberate, narrow exception to `display_protocol.h`'s own
  "the kernel never interprets these opcodes" stance (that file's own
  doc comment now says so explicitly) — the kernel doesn't branch on
  what `DISPLAY_OP_GO` MEANS, it just constructs the same wire value a
  peer client would have sent.
- **`pulse_app.c`'s own go-forward to the clock app is now guarded
  (`if (clock_app_pid != 0)`).** A dynamically spawned pulse app
  instance has no chain to forward anything down (its own boot
  message's `fields[1]` is left 0 by the shell) — forwarding to pid 0
  unconditionally would misdeliver into the bootstrap kernel thread's
  own inbox (task id 0): not a crash, but a real correctness bug (a
  stray, never-read message silently occupying a queue slot forever).
- **A real, root-caused (not assumed) QEMU test-harness finding:
  dynamically spawned windows are placed in the genuinely empty
  horizontal gap between client B's right edge (350) and the
  pulse/clock apps' left edge (650), not deliberately overlapping a
  boot-time window as an earlier version of this milestone did.**
  Investigating why an overlapping placement's own screendump-based
  smoke test intermittently showed a region still displaying a
  long-since-superseded window's stale content, a kernel-side
  readback (`fb_read_rect()`, immediately after the exact same write)
  confirmed the real framebuffer memory was ALREADY correct — proving
  this was a QEMU `-display none` / monitor `screendump` display-
  refresh artifact for a region that hadn't been actively redrawn in a
  while, not a kernel bug. (CLAUDE.md's "diagnose first" discipline
  applied all the way to the actual root cause, via a real evidence
  trail, not stopping at the first plausible guess — the same
  standard ADR 0032's own diagnosis already set.) Overlapping
  compositing itself is NOT broken (dragging a window into an overlap,
  Milestone 31, already proves this, and remains untouched) — picking
  a non-overlapping default position sidesteps needing to work around
  this specific QEMU-side artifact in every future screendump-based
  smoke test, without giving up anything this milestone needs to
  prove.

## Rejected alternatives
- **A general N-window architecture with dynamic (heap-allocated)
  storage.** Rejected — a fixed-capacity array sized generously for
  this hobby kernel's scale is the SAME pattern already established
  twice elsewhere in this codebase; no kernel heap allocation is
  needed or wanted for a bounded, small resource like this.
- **Restructuring `pulse_app.c`/`clock_app.c` to make their go-signal
  wait conditional/optional**, so a dynamic spawn wouldn't need the
  kernel to construct a `DISPLAY_OP_GO`. Rejected — see Decision;
  reusing both programs completely unmodified, with the kernel
  satisfying an EXISTING wait rather than adding a new code path to
  two already-working programs, is the smaller, safer change.
- **Reclaiming a closed dynamic window's array slot for reuse by a
  future spawn.** Rejected for this milestone — `WINDOWS_CAPACITY` (8)
  is generous enough that slot exhaustion isn't a practical concern
  yet, and slot reuse would need its own correctness reasoning (stale
  `pid`/`va` references, z_order bookkeeping) that nothing here
  actually needs solved today. Flagged explicitly as a real, known
  gap, not silently ignored (see Known limitations).
- **Debugging the screendump discrepancy by guessing** (e.g.
  "add a delay before screendump," "redraw twice," or simply loosening
  the smoke test's pixel assertion). Rejected — each would have masked
  a symptom without knowing whether a real kernel bug existed. The
  kernel-side `fb_read_rect()` readback was added specifically to get
  a DEFINITIVE, first-principles answer (matching CLAUDE.md's "diagnose
  first" discipline) before deciding this was a test-environment
  artifact, not a correctness gap needing a kernel-side fix.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-35
  marker unchanged.
- **`tests/qemu/test_dynamic_spawn_selftest.sh` (new)**: types real
  `spawn pulse` and `spawn clock` through the actual PS/2 keyboard path
  (monitor `sendkey`, the same technique `test_shell_selftest.sh`
  already established), confirms both new windows genuinely render
  (screendump, checked against each program's own known palette/digit
  colors in a region that can only belong to that specific window),
  then closes the dynamically spawned pulse window with a real
  injected click and confirms the same three-independent-facts exit
  proof `test_window_close_exit_selftest.sh` (Milestone 34) already
  established, plus that the UNRELATED dynamically spawned clock
  window is completely unaffected.
- Booted clean under real KVM acceleration with the full spawn/spawn/
  close/exit/reap sequence, zero `#GP` faults.
- All thirty pre-existing smoke tests and all four host test suites
  re-verified passing on a clean rebuild.
- Root-caused (not assumed) the screendump-staleness finding using a
  kernel-side `fb_read_rect()` readback taken immediately after a
  `sys_fb_present()` write to the exact same coordinates, confirming
  real framebuffer memory correctness independent of what QEMU's own
  `screendump` monitor command happened to show at that moment.

## Known limitations (accepted for this milestone only)
`WINDOWS_CAPACITY` is a fixed 8 (4 boot-time + 4 dynamic) — spawning a
9th window fails cleanly (the existing `granted_w == 0` failure path
every client already handles) rather than growing further. A closed
dynamic window's array slot is never reclaimed for reuse by a later
spawn — `windows_used` only ever grows, so a boot that spawned and
closed several windows would eventually exhaust `WINDOWS_CAPACITY`
even though some of those slots hold nothing but a closed, inert
window. Only `pulse` and `clock` are spawnable — a real path-based
`execve` (launching an ARBITRARY program) remains blocked on the
filesystem non-goal, unchanged. No "list running windows" or
"relaunch by pid" shell command exists yet.
