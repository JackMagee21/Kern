# ADR 0034: a real client exit/close protocol

## Status
Accepted and verified — closing a window now tells its owning client
to actually exit, and the scheduler's existing reaper genuinely
reclaims it (task_t, kernel stack, address space, and its shm
reference), rather than leaving a still-running client spinning
forever with nothing displaying it. This closes the specific gap
Milestone 33's own Known limitations flagged: the pulse app
(`kernel/user/pulse_app.c`) is the first client that could actually
still be alive when its window closes. `make run` boots the real ISO
unchanged; a new smoke test and all twenty-nine pre-existing ones
pass. Booted clean under real KVM acceleration (ADR 0032/its own
follow-up fix) with the exact close/exit/reap sequence, zero `#GP`
faults.

## Context
`future.md`'s own "Reasonable next steps" listed this as the most
natural next piece of GUI-arc-adjacent work: clients A/B
(`display_client_a.c`/`_b.c`) already exit on their own, long before a
close is even possible, so closing their windows was always harmless.
The pulse app doesn't -- it runs forever by design (Milestone 33's
whole point). Before this milestone, `handle_click()`'s close branch
only ever set `window_t.closed = 1` and stopped compositing that
window; the underlying client process kept running, burning a
scheduler timeslice every round-robin turn forever, with its
`task_t`/kernel stack/address space/shm reference never reclaimed.

## Decision

- **One new protocol message, not a teardown subsystem.**
  `DISPLAY_OP_EXIT` (`kernel/user/display_protocol.h`, opcode 7) is a
  no-fields ping, server -> client: "you, specifically, should exit
  now." Sent to `window_t`'s own new `pid` field (filled in once, at
  grant time, from the client's own `DISPLAY_OP_REQUEST`'s
  `sender_pid`) -- the server already learns this for free from every
  handshake, so tracking it needed no new message exchange.
- **Safe to send even to a client that's already gone.** Clients A/B
  exit immediately after their initial present, long before a user
  could ever click their close button. `sys_ipc_send()` to a pid the
  scheduler no longer recognizes (`scheduler_find_task()` returns
  `NULL`) just fails, silently, the same as it already does for any
  other stale-pid send -- and since pids are never recycled
  (`scheduler.c`'s own `next_task_id` invariant), this can never be
  misdelivered to an unrelated later process. No "is this client still
  alive" check was needed before sending; the existing failure path
  already handles it correctly.
- **A new non-blocking syscall, `sys_ipc_try_recv`
  (`SYS_IPC_TRY_RECV`, number 13) -- exposing an EXISTING kernel
  primitive, not building a new one.** `kernel/ipc/msgqueue.c`'s
  `ipc_try_recv()` already existed (it's what `sys_ipc_recv`'s own
  blocking loop calls internally) -- `sys_ipc_recv`'s own doc comment
  had explicitly flagged a non-blocking variant as YAGNI "until
  something actually needs one." Something now does: the pulse app's
  animation loop can't give up its own pacing to block in
  `sys_ipc_recv` waiting for an exit message that might never come.
  `sys_ipc_try_recv` is the thinnest possible wrapper around the
  already-existing primitive -- same validation, same struct, no new
  kernel-side bookkeeping.
- **The client polls once per animation frame, not once per
  `sys_nop`.** Checking after every single `sys_nop` in the
  `FRAME_DELAY_NOPS` spin would turn a cheap poll into the dominant
  cost of the whole loop; once per frame (the same cadence the color
  cycle itself already advances at) is enough to make the close feel
  immediate to a person clicking it, without the poll ever being the
  loop's bottleneck.
- **The reaper needed zero changes.** `task_create_user_image()`
  already sets `parent_id = 0` for the pulse app (same as
  `display_server_process` and clients A/B) -- an ordinary orphan
  process. `scheduler_exit_current()` (called by `sys_exit`, which the
  pulse app now calls) and the existing reaper's orphan path
  (`scheduler_unregister_task()` + `kfree()`) already handle this
  exactly the way they handle every other process's exit; the pulse
  app calling `sys_exit(0)` needed no new teardown logic anywhere.
- **The frame-leak/permanent-canvas-page accounting (Milestone 33,
  `DISPLAY_SERVER_PERMANENT_CANVAS_PAGES`) needed no change.**
  `kernel_main`'s own self-tests all run automatically during boot,
  strictly BEFORE the shell prompt appears -- this milestone's close/
  exit path only ever runs when something (a real user, or this
  milestone's own new smoke test) injects a click AFTER the shell
  prompt. The DEFAULT boot path is completely unchanged: nobody closes
  the pulse app's window during boot's own automatic self-tests, so it
  keeps running and holding both its shm references exactly as
  Milestone 33 already accounted for. Verified by reasoning through the
  ordering (self-tests happen before the shell prompt; the new close
  path can only happen after it), not assumed.

## Rejected alternatives
- **A general "every client must periodically poll for control
  messages" convention.** Rejected -- clients A/B never need this (they
  exit long before a close is possible); building a shared polling
  convention for a property only one current client has would be
  machinery ahead of an actual second consumer (CLAUDE.md: don't build
  for a hypothetical future need).
- **Reclaiming the window's shm/process resources directly from
  `handle_click()`, without a client-side exit at all** (e.g. the
  server force-unmapping the client's shm and leaving its task
  orphaned in a limbo state). Rejected -- the client's own `sys_exit()`
  is the existing, correct, single way a process's resources get torn
  down in this kernel (address space destruction, kernel stack
  freeing, shm reference drop, reaper collection); reimplementing any
  piece of that from the server's side would duplicate logic that
  already exists and is already tested.
- **A blocking `sys_ipc_recv()` with a kernel-enforced timeout**
  (return after N ticks if nothing arrived). Rejected as needless
  complexity -- `ipc_try_recv()` already existed as a plain, immediate,
  non-blocking primitive; a timeout-based blocking variant would need
  new kernel-side timer plumbing to solve a problem the existing
  primitive already solves for free.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-33
  marker unchanged.
- **`tests/qemu/test_window_close_exit_selftest.sh` (new)**: injects a
  real click on the pulse app's close button, then confirms THREE
  independent facts, not just one self-reported marker: the server's
  own `[OK] display server: closed window 0x2`, the CLIENT's own `[OK]
  pulse app: received exit request, exiting` (proof the message was
  actually received and acted on, from the other side of the
  protocol), and `exited and was reaped` appearing strictly AFTER the
  shell prompt (proof the scheduler's reaper genuinely collected the
  process, not just that it printed a message and kept spinning). A
  final screendump confirms none of the pulse app's four palette
  colors remain visible anywhere on screen.
- Booted clean under real KVM acceleration with the exact same
  close-click sequence: `[OK] display server: closed window 0x2` ->
  `[OK] pulse app: received exit request, exiting` -> `[OK] process
  0000000000000007 exited and was reaped`, zero `#GP` faults --
  specifically exercised because this exact code path (a close-click
  event delivered mid-boot, after the shell prompt) is precisely the
  territory the real-hardware SS-corruption bug (ADR 0032 and its own
  follow-up fix) lived in.
- All twenty-eight pre-existing smoke tests and all four host test
  suites re-verified passing on a clean rebuild (unaffected --
  `test_window_chrome_selftest.sh`'s own close-of-client-A scenario
  still sends `DISPLAY_OP_EXIT` to client A's now-long-exited pid,
  which correctly no-ops via the existing stale-pid failure path, the
  same as before this milestone).

## Known limitations (accepted for this milestone only)
No general "list running programs" or "relaunch a closed window"
exists -- once closed, that window's slot stays empty for the rest of
the boot (dynamic window creation remains its own separate,
un-started `future.md` candidate). The server's OWN second shm
reference to a closed window's canvas is still never dropped (the
server itself never exits) -- this was already true and already
accounted for (Milestone 33's `DISPLAY_SERVER_PERMANENT_CANVAS_PAGES`)
before this milestone; closing a window now drops the CLIENT's own
reference for the first time, but the server's permanent one is an
unrelated, pre-existing, and intentional design property (the server
needs to keep the canvas mapped in case it's ever asked to recomposite
that region again). Clients A/B still don't listen for
`DISPLAY_OP_EXIT` at all (they don't need to -- they're already gone by
the time a close is possible) -- if a future milestone ever makes A/B
long-running too, they'd need the same per-frame poll this milestone
added to the pulse app.
