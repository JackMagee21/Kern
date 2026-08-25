# ADR 0029: real input-driven click routing (hardware event → userspace IPC)

## Status
Accepted and verified — `make run` boots the real ISO; a genuine PS/2
left-click, decoded by the real mouse driver from an actual IRQ12
report, is routed via IPC to a specific, genuinely separate ring-3
process (`kernel/user/input_focus_demo.c`) — the first time this
kernel has ever delivered a hardware event to userspace at all. Proven
with a real QEMU monitor-injected click (`mouse_move`/`mouse_button`,
the same technique Milestones 16/23 already established), landing at
the exact expected screen position, read straight out of the routed
message the kernel itself printed. All twenty-seven smoke tests (26
pre-existing plus the new `test_input_focus_selftest.sh`) and all four
host test suites pass. `-d int,cpu_reset` trace unchanged from
Milestone 28. Booted clean on the first real attempt for the core
mechanism; a real structural conflict with this project's own
deterministic-boot invariant was found and resolved in review, before
it could ever manifest as a hung test suite — see Decision.

## Context
Sixth step of the GUI arc (`Desktop.md`) — the second half of the
milestone 5 arc item Milestone 28 deliberately split in two (see ADR
0028's own Decision): real click-driven input routing, the genuinely
new subsystem work Milestone 28 explicitly deferred. Every prior
consumer of `kernel/drivers/mouse.c`'s decoded events
(`cursor_poll()`, the shell's `mouse` command) is kernel-side code;
nothing in this kernel has ever handed a hardware event to a ring-3
process before.

## Decision

- **Scope: prove the delivery mechanism in isolation, NOT "click
  raises a window."** Wiring this all the way through to
  `kernel/user/display_server.c` actually raising/re-presenting a
  clicked window would require redesigning that server's own lifecycle
  from "serve 2 clients, then exit" (Milestone 27/28's own deterministic,
  self-contained design) into a persistent event loop — a second,
  genuinely separate piece of work (CLAUDE.md: one subsystem per
  change). This milestone proves ONLY the new mechanism: a real click
  event reaching a specific ring-3 process via IPC. Actually making
  the display server act on one is explicitly deferred to its own
  later milestone.
- **A new, SEPARATE subscription concept (`sys_input_subscribe`/
  `input_focus_pid`), not a reuse of `fb_owner_pid`.** "Who owns the
  framebuffer" and "who should receive input events" are, in general,
  different questions — a future window manager might route input
  through a different process than the one that owns the pixels.
  Reusing `fb_owner_pid` would also have created an artificial
  ordering dependency between this milestone's demo and Milestone
  27/28's display demo (both created by `kernel_main`) over which one
  gets to be "the" framebuffer owner. Same kernel-enforced exclusivity
  pattern as `sys_fb_acquire()` (first caller wins, checked by pid, a
  repeat call from the same process fails) — proven, minimal, and
  already established, so reused as a PATTERN without reusing the
  actual state.
- **A real structural conflict, found in review before ever booting:
  a process that blocks forever for external input cannot be part of
  `kernel_main`'s own reap-count self-test gate.** The first design
  draft created `input_focus_demo` alongside the other ring-3 demo
  processes (Milestone 27/28's own display pair, the IPC pair, etc.) —
  all of which are counted in a `while (scheduler_reaped_count() < N)`
  gate `kernel_main` blocks on before proceeding to later self-tests
  and finally the shell. Since `input_focus_demo` only ever exits after
  a REAL external click (impossible for a plain headless boot with no
  QEMU monitor injection to ever provide), including it in that gate
  would have hung EVERY OTHER smoke test in this suite forever, not
  just this milestone's own. Caught by reasoning through what "blocks
  forever without external input" actually implies for the shared gate
  every other test depends on, before ever running QEMU — the same
  prospective-diagnosis discipline Milestone 20's ADR already
  established. Fixed by creating `input_focus_demo` BEFORE
  `kernel_main`'s frame-leak baseline snapshot (`frames_before_processes`),
  alongside the genuinely permanent kernel threads (`demo_task_a/b`,
  `block_test_blocker/waker`) rather than the bounded, self-exiting
  demo processes — its own frame footprint becomes part of what the
  leak check compares FROM, so neither the reap-count gate nor the
  leak check ever depends on it exiting during a normal boot. It is a
  genuinely new category (a RING-3 process, unlike the kernel threads
  it now sits alongside, deliberately excluded from the reap gate) —
  documented explicitly at its own creation site so a future reader
  isn't confused by the placement.
- **No `kernel_main` self-test panics on `input_router_get_click_count()`
  being zero**, unlike every other "prove the new path was exercised"
  counter this project has added (`syscall_get_exec_count()`,
  `syscall_get_fb_present_count()`, etc.). Those could all rely on a
  plain headless boot deterministically exercising the new path by
  construction (a self-contained demo needs no external input). This
  milestone's whole subject is REAL, asynchronous external input —
  nothing in this kernel can synthesize a genuine PS/2 click from
  inside `kernel_main` itself; only the QEMU monitor (or real hardware)
  can. A hard panic-on-zero check here would make every OTHER smoke
  test that doesn't happen to inject a click fail — the counter and the
  kernel's own routed-click log line exist purely for
  `test_input_focus_selftest.sh` (the one test that actually injects
  a click) to verify against.
- **Click delivery is reported by the KERNEL, not the receiving
  process, since the userspace runtime has no way to format numbers.**
  `kernel/user/rt/` (Milestone 24) has no `printf`-equivalent — every
  existing demo only ever prints fixed literal strings. Rather than add
  integer-to-string formatting to the runtime just for this one
  self-test's benefit, `input_router.c` itself logs the exact routed
  `(x, y)` via the kernel's own existing `console_write_hex()` — the
  same trusted-kernel-side-reporting role it already plays for every
  other "prove precisely what happened" marker in this codebase.
- **Click EDGE detection lives in `cursor.c`, not a new dedicated mouse
  queue.** The PS/2 `left` field is a LEVEL (current physical button
  state per report), not an edge — turning it into a genuine
  false→true transition needs a `left_was_down` comparison somewhere.
  `cursor.c` already drains every cursor-queue event (including button
  state) and already tracks the authoritative on-screen cursor
  position a click needs to be reported at, so it's the natural,
  already-existing place to do this — a THIRD independent mouse queue
  (mirroring Milestone 23's own debug/cursor split) was considered and
  rejected as unneeded: nothing else needs its own independent view of
  raw button levels, only the edge, which `cursor.c` can already derive
  and forward.

## Rejected alternatives
- **Reusing `fb_owner_pid` as the input-routing target.** Rejected —
  see Decision; conflates two genuinely different concerns and creates
  an artificial ownership race between this milestone's demo and the
  display server.
- **Wiring this straight into `display_server.c`** so a click actually
  raises a window in this same milestone. Rejected — see Decision's
  scope reasoning; a real, separate redesign of that server's own
  lifecycle.
- **A `kernel_main` panic-on-zero self-test for click delivery.**
  Rejected — see Decision; would break every OTHER headless test that
  doesn't inject external input.
- **A third independent mouse event queue** for click-edge detection.
  Rejected — `cursor.c` already sees everything a click-edge detector
  needs; a new queue would just be unused duplication.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-28
  marker unchanged, plus: `[OK] input focus demo process created...`
  (created early, alongside the kernel threads), `[OK] input focus
  demo: subscribed to hardware input events`, `[OK] input focus demo: a
  second sys_input_subscribe correctly failed...` — all during the
  SAME boot as every other test, with the demo then sitting genuinely
  and harmlessly blocked for the rest of it (confirmed: it does NOT
  appear in the reap-count log during a plain headless boot, and every
  one of the 26 pre-existing tests still passes unchanged, still
  showing exactly 10 reaps).
- **`tests/qemu/test_input_focus_selftest.sh` (new)**: after the shell
  prompt confirms boot has settled, injects a REAL `mouse_move 40 0` +
  `mouse_button 1`/`0` through the QEMU monitor (the identical
  technique `test_mouse_selftest.sh`/`test_framebuffer_selftest.sh`
  already proved decodes correctly), then asserts: (1) the kernel's own
  `input_router.c` log line reports the click routed to the demo's
  exact pid, at the exact expected position (screen-center-x + 40,
  read from the framebuffer's own self-reported negotiated dimensions
  in the boot log, not a hardcoded assumption — Milestone 23's own
  `fb_init()` doc comment: never hardcode the negotiated mode); (2) the
  demo process itself reports receiving a message with the correct
  opcode; (3) the demo then actually exits and is reaped (11 total —
  the same 10 baseline every other test expects, plus this one, which
  ONLY this test's own injected click makes possible).
- `-d int,cpu_reset` trace: unchanged from Milestone 28 (1 `#BP`, 3
  `#PF`, zero kernel-caused resets).
- All twenty-six other smoke tests and all four host test suites
  re-verified passing on a clean rebuild. Booted 3 additional repeat
  times — identical marker sequence, exact reap count (10, unchanged
  for a plain headless boot) every time.

## Known limitations (accepted for this milestone only)
Exactly one input-focus subscriber, ever, for the whole boot — no
unsubscribe, no re-subscription if the current subscriber exits (a real
gap: once `input_focus_demo` itself exits, per Milestone 29's own click
test, NO future click will ever be routed anywhere for the rest of that
boot, since `input_focus_pid` is never reset — matching the identical,
already-accepted limitation `fb_owner_pid` has, ADR 0027's Known
limitations). Only left-click DOWN edges are routed — no right/middle
button, no click-up/release event, no drag. The mechanism proven here
is NOT yet wired to anything that acts on it visually (no window
actually raises/focuses in response) — that's `Desktop.md`'s own next
arc item, building directly on this milestone's delivery path.
