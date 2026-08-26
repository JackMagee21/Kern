# ADR 0037: serial as the debug log, a clean on-screen desktop

## Status
Accepted and verified — the on-screen framebuffer console now shows
only what the actual interactive OS surface needs (the shell's own
banner/prompt/echo, plus the GUI desktop's own pixels), not the ~100
lines of boot-time `[OK]` self-test markers and the ongoing per-click/
per-reap chatter that used to scroll across it. Every byte of that
diagnostic detail still reaches serial in full, unchanged — every
existing QEMU smoke test greps that log, never the screen, and all
thirty-one keep passing. One real regression found and fixed during
verification (see Decision's last bullet).

## Context
The user asked directly: keep serial as a full debug log, but make the
actual on-screen OS clean. Before this milestone, `console_write()`
fanned every write out to both serial AND the graphics-framebuffer
text console (`console.h`'s own doc comment: "so boot self-tests and
panics are visible on real hardware with no serial cable attached").
That reasoning is still correct for genuine failures, but it meant
routine, successful, EXPECTED output — kernel_main's own ~100 `[OK]`
boot markers, the reaper's per-process reap line, input_router.c's
per-click/drag/release trace, and every ring-3 GUI program's own
`sys_write()` diagnostic markers — filled the reserved console region
with a wall of scrolling text a person actually looking at the desktop
never needed to see, and kept growing during normal use (every mouse
drag alone can generate dozens of trace lines).

## Decision

- **A new serial-only pair, `console_log`/`console_log_hex`
  (`console.c`/`.h`), alongside the EXISTING dual-output
  `console_write`/`console_write_hex` -- not a replacement for them.**
  The real design question this milestone answers is WHICH callers use
  which, not deleting either path.
- **The line: genuine unrecoverable failures and the actual interactive
  OS surface stay dual-output; routine/expected/frequent chatter moves
  to serial-only.** Concretely:
  - `panic.c` and the bulk of `exceptions.c`'s own field-by-field fault
    dump: UNCHANGED at first glance, but see the refinement below --
    now serial-only for anything that RESUMES (the #BP self-test,
    the already-silent resolved COW #PF case), dual-output only for
    what's actually about to halt the machine forever.
  - `scheduler.c`'s `scheduler_dump_switch_diag()` (the Milestone 32
    flight recorder): kept dual-output, unconditionally -- it only ever
    runs immediately before a real `#GP` panic, so it's part of the
    same "genuine failure" case, not routine chatter.
  - `shell.c`: entirely unchanged -- its prompt/echo/command output IS
    the actual interactive OS a person at the keyboard is meant to see.
  - Everything else: `kernel.c`'s ~120 `console_write`/
    `console_write_hex` call sites (every one of them a boot-time
    self-test marker; kernel_main has no on-screen content of its own
    -- that's `shell_run()`'s job, a separate function, left untouched)
    mechanically converted to `console_log`/`console_log_hex`.
    `input_router.c`'s per-event trace and `scheduler.c`'s reaper line
    converted the same way, each with its own comment explaining why
    (both fire during NORMAL, ONGOING desktop use, not just boot).
- **`sys_write` (`syscall.c`) changed from `console_putc` to
  `serial_putc` directly -- one change covers every ring-3 program.**
  Every current ring-3 process (`display_server.c`, both display
  clients, the pulse/clock apps, `hello.c`, the fork/exec/IPC demos)
  uses `sys_write()` exclusively for diagnostic/self-test markers,
  never as on-screen TEXT a user is meant to read -- a GUI client's
  actual visible output is pixels, via `sys_fb_present()`. Changing the
  ONE shared kernel syscall handler, rather than editing every
  `kernel/user/*.c` file individually, handles all of them uniformly
  and correctly by construction (there's no future ring-3 program that
  would need a DIFFERENT answer without also needing a real on-screen
  text UI mechanism this kernel doesn't have yet).
- **A real refinement found while verifying the visual result, not
  planned in advance: the Milestone 2 `#BP` self-test's own full
  20-field register dump was STILL appearing on screen** (unaffected
  by the `sys_write`/`kernel.c` changes -- `exceptions.c` was never
  touched by those). Investigating why the screen still showed a large
  block of text after the rest of the cleanup found this: `#BP` is an
  EXPECTED, RESUMED success (the whole point of int3 as a debugging
  primitive, and Milestone 2's own self-test), not a failure -- it had
  no more business filling the screen than any other routine
  diagnostic, it was simply invisible before, buried under ~100 other
  boot lines. Fixed by moving the field-by-field dump itself to
  `console_log` (still reaches serial in full, unconditionally, for
  EVERY exception including `#BP`), and adding a NEW, separate, SHORT
  dual-output summary line (`"[PANIC] <name> -- system halted, see
  serial log for full diagnostic detail"`) printed only immediately
  before the closing `for(;;) hlt` -- i.e. only for an exception that's
  actually about to halt the machine forever. This is the one place a
  screen with no serial cable attached still needs SOME indication a
  real crash happened (CLAUDE.md safety rule 6, "never fail silently,"
  applied to the on-screen experience specifically) without repeating
  the whole detailed dump that already went to serial.
- **A real, found-in-verification test regression, not a kernel bug:
  `test_dynamic_spawn_selftest.sh`'s own serial-log assertion checked
  for `"> spawn clock"` as one adjacent substring.** `sys_write`'s new
  direct-to-`serial_putc` path has a different performance profile than
  the old per-byte `console_putc` (which also rendered a glyph into the
  framebuffer each time) -- shifting the relative timing of the shell's
  own prompt echo against the display server's own concurrent
  diagnostic output (both write to the same serial byte stream) just
  enough that the two could now interleave differently than they used
  to for this one specific test scenario (the only existing test that
  triggers a spawn -- and thus concurrent display-server output --
  while typing an adjacent shell command). The prompt-plus-typed-text
  adjacency was never a real invariant this kernel guarantees; fixed by
  loosening the assertion to just `"spawn clock"` (still proves the
  keystroke echo genuinely happened -- no other process could ever
  print that exact literal text -- without depending on exact
  byte-level interleaving against an unrelated concurrent process).
  `test_shell_selftest.sh`/`test_reboot_selftest.sh` use the same
  `"> command"` pattern but were verified NOT to need the same fix --
  neither test ever triggers a spawn/click/close event, so nothing
  concurrent is emitting console output during their own checks.

## Rejected alternatives
- **A boot splash or progress indicator**, replacing the wall of text
  with SOME visible boot-time content rather than a blank screen until
  the shell/GUI appear. Rejected as scope beyond what was asked --
  "clean" was the request, not "prettier"; a blank screen briefly, then
  the shell prompt and the real desktop, already satisfies it without
  a new rendering subsystem.
- **Moving `panic.c`/`exceptions.c`'s full dump to serial-only
  entirely**, with no on-screen trace of a crash at all. Rejected --
  would silently regress real-hardware diagnosability (no serial
  cable, no indication anything went wrong) that this exact
  console.h fan-out design existed to provide in the first place;
  the short one-line summary added instead keeps that property while
  still meeting "clean" for the routine, successful path.
- **A per-message "is this routine or not" flag threaded through
  `sys_write`/`console_write`'s own signature**, letting a caller
  choose dual vs. serial-only output per call. Rejected as needless
  complexity -- the actual population of callers cleanly splits into
  exactly two buckets (genuine-failure-or-interactive vs.
  everything-else) with no caller needing to switch between them
  dynamically; two separate function pairs, chosen once per call site
  based on which bucket it's in, is simpler and doesn't add a
  parameter to code that's already deliberately minimal.
- **Silently loosening the broken test assertion without figuring out
  WHY it broke.** Rejected -- CLAUDE.md's "diagnose first" discipline:
  understanding that this was a genuine, previously-latent interleaving
  non-guarantee (not a kernel correctness regression) was necessary
  before deciding the TEST's assertion was the thing that needed
  fixing, not the kernel.

## Verification
- `make run` (real toolchain) boots and prints every existing
  `[OK]`/`[FAIL]` marker to serial, unchanged, verified via a real
  headless boot's captured serial log (identical content to before
  this milestone).
- A real QEMU screendump, taken after the shell prompt appears on a
  quiet boot (no input injected), confirms the console region (y
  0-480) contains grey text ONLY on rows 8-22 -- exactly the two lines
  `shell_run()` prints (`"kernel shell..."` banner + `"> "` prompt) --
  down from content spanning essentially the whole region before this
  milestone.
- All thirty-one QEMU smoke tests and all four host test suites
  re-verified passing on a clean rebuild, including the fixed
  `test_dynamic_spawn_selftest.sh`.

## Known limitations (accepted for this milestone only)
No boot splash or visible progress indicator during the (fast, sub-
second in emulation) boot sequence -- the screen is simply blank until
the shell prompt and GUI windows appear, matching what was actually
asked for ("clean"), not a new visual design. A genuine kernel panic's
on-screen summary is a single line with the exception name only, not
the fault address/faulting instruction -- deliberately terse (the full
detail is one `grep` away in serial); if real-hardware debugging ever
needs more than that visible without a serial cable, this would need
revisiting, but nothing currently motivates it.
