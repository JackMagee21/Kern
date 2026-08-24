# ADR 0016: PS/2 mouse driver

## Status
Accepted and verified — `make run` boots the real ISO, IRQ2 (the
master PIC's cascade line, required for ANY slave-PIC IRQ8-15 to ever
reach the CPU) and IRQ12 are correctly unmasked, and the shell's new
`mouse` command correctly decodes real synthetic PS/2 mouse movement
and button events injected through QEMU's monitor. Unlike every other
milestone this session (ADRs 0010-0015, all correct on the first real
boot), this one hit a genuine, non-obvious bug that took real hardware-
level diagnosis to find and fix — see Verification for the full trail.

## Context
Eighth step of the post-Milestone-8 "build this into an OS" inventory
— "mouse," the last hardware item from the original list that doesn't
touch a flagged non-goal (storage/filesystem and ACPI shutdown remain
flagged, awaiting the user's decision; SMP/networking are explicit
non-goals). There is still no graphics/framebuffer output in this
kernel (VGA text mode only, ADR 0008), so there's no cursor to actually
draw — this milestone proves the PS/2 mouse protocol can be decoded
correctly, deferring cursor rendering to whenever a graphics milestone
exists.

## Decision
- **The PS/2 controller's second ("auxiliary") port, IRQ12** — the
  standard legacy mouse interface, using the SAME 8042 controller
  `keyboard.c` already drives (port `0x60` for data, `0x64` for
  controller commands/status). Ports, controller commands (`0xA8`
  enable aux port, `0x20`/`0x60` read/write the configuration byte,
  `0xD4` write-to-aux prefix), the configuration-byte bit layout, and
  the standard 3-byte streaming packet format all verified against the
  OSDev.org "Mouse Input" wiki article's documentation of this
  decades-standard interface — the same class of source this codebase
  already treats as authoritative for legacy hardware (ADR 0005, ADR
  0013, ADR 0014, ADR 0015).
- **Lives in `kernel/drivers/mouse.c`, separate from `keyboard.c`**,
  despite sharing the controller — distinct IRQ line, distinct packet
  format, distinct device-level command sequence; matches how
  `reboot.c` stayed separate from `keyboard.c` too (ADR 0015) even
  though it also uses port `0x64`.
- **IRQ2 (the master PIC's cascade line) had to be explicitly
  unmasked alongside IRQ12** — a real, easy-to-miss detail: IRQ8-15
  live on the SLAVE 8259, whose own INTR output is wired into the
  MASTER's IRQ2 input. If IRQ2 stays masked (as it was, since nothing
  before this needed a slave-PIC line), the slave's signal can never
  reach the CPU at all, regardless of IRQ12's own mask bit. Caught by
  reviewing `pic.c`'s cascade wiring (ICW3's `0x04`/`0x02` values) before
  writing the driver, not discovered by a live hang.
- **A small, dedicated fixed-capacity event queue in `mouse.c`**
  (`mouse_event_t[32]`, plain head/tail indices), not a reuse of
  `libk/ring_buffer.c` — that module is `char`-specific; a second,
  struct-typed instantiation wasn't worth generalizing it for yet
  (CLAUDE.md: don't add abstractions beyond what's needed).
- **`dx`/`dy` are reported in raw PS/2 wire convention (positive `dy` =
  UP), not converted to screen coordinates** — there is no display
  consumer yet to have an opinion about sign convention; a future
  cursor-rendering milestone makes that call when it exists, not this
  one. Documented explicitly in `mouse.h` rather than left as an
  unstated assumption.
- **A `mouse` shell command**, matching every other driver-visibility
  command added this session (`date`, `reboot`) — blocks until one
  event is available, then prints its decoded `dx`/`dy`/button state.
- **An explicit filter for the device's own ACK (`0xFA`)/RESEND
  (`0xFE`) response bytes, checked before the packet-start resync
  test** — required by a real bug found during verification (see
  below): the ACK byte's bit 3 happens to be set, so without this
  explicit check it can be mistaken for a legitimate first packet byte
  if it arrives asynchronously, corrupting the next real packet's
  framing.

## Rejected alternatives
- **Reusing `libk/ring_buffer.c` by widening it to a generic
  `void*`/templated element type** — rejected as premature
  generalization for a single second use case; a plain fixed-size
  struct array is simpler and just as correct here.
- **Converting `dy` to screen-down-positive at the driver layer** —
  rejected: with no display/cursor consumer to define "screen" at all,
  baking in a sign convention now is a guess a future graphics
  milestone would just have to un-guess. Left as the documented raw
  wire value instead.
- **Trusting `wait_output_full()`/a single drain pass alone to keep the
  stream clean** — tried first, didn't fully fix the real bug (the
  stray ACK can arrive well after any pre-streaming drain point, see
  Verification); kept as cheap complementary defense-in-depth, but the
  actual fix is the explicit ACK/RESEND byte-value check in the
  streaming parser itself, which is correct regardless of exactly when
  the stray byte shows up.

## Verification
- **A real bug, found and fixed, not present on the first attempt**
  (the first of this session's eight milestones to need this):
  the very first live test showed the shell's `mouse` command hanging
  forever after an isolated `mouse_move` injected via the QEMU monitor,
  despite multiple back-to-back moves working fine in an earlier,
  differently-shaped manual test. Diagnosed methodically rather than
  guessed: instrumented `mouse_irq_handler` with a temporary per-byte
  serial print, which showed a stray `0xFA` byte arriving via IRQ12
  well after `mouse_init()` had already completed and consumed both
  expected handshake ACKs. `0xFA`'s bit 3 (the packet framing spec's
  "always 1" resync bit) happens to be set, so it was silently accepted
  as a legitimate first packet byte, shifting the next 2 real bytes
  into the wrong positions and corrupting the packet's decoded `flags`
  byte -- which then hit the overflow check and got discarded outright,
  leaving the parser out of sync with no further packets ever arriving
  to resynchronize it (QEMU's monitor `mouse_move` sends one packet per
  invocation, not a continuous stream). First fix attempt (draining the
  output buffer once at the end of `mouse_init()`) did NOT fix it --
  confirmed by rerunning the same instrumented test, showing the same
  stray `0xFA` still arriving via IRQ12 -- meaning the byte genuinely
  becomes available asynchronously, sometime after `mouse_init()`
  returns (most plausibly whenever QEMU's virtual mouse backend is
  first actually touched by a monitor command, not a fixed point during
  boot). Real fix: explicit `MOUSE_RESP_ACK`/`MOUSE_RESP_RESEND` value
  checks in the streaming parser itself, which is correct regardless of
  when the stray byte shows up. Confirmed fixed by rerunning the exact
  same instrumented scenario: the real 3-byte packet (`0x08 0x0f 0x00`)
  now decodes to `dx=15 dy=0`, matching the injected `mouse_move 15 0`
  exactly. Diagnostic instrumentation removed once the fix was
  confirmed.
- `make run` (real toolchain) boots and prints `[OK] pic/pit/keyboard/
  mouse initialized, IRQ0+IRQ1+IRQ2+IRQ12 unmasked`, with every
  Milestone 1-15 marker unchanged. `info pic` (QEMU monitor) confirmed
  the actual hardware mask state directly: `pic0: imr=0xf8` (bits 0/1/2
  clear -- IRQ0/1/2 unmasked), `pic1: imr=0xef` (bit 4 clear -- IRQ12
  unmasked) -- read from live emulated register state, not inferred
  from the kernel's own claim.
- The shell's `mouse` command was exercised with REAL injected PS/2
  input (QEMU monitor `mouse_move`/`mouse_button`, actual virtual
  hardware events, not a shortcut around the driver) -- confirmed both
  movement decoding (including the expected raw-PS/2-vs-screen Y-axis
  sign inversion: a screen-down `mouse_move 0 20` correctly decoded to
  raw `dy=-20`) and button decoding (`mouse_button 1`/`0` correctly
  producing `L=1`/`L=0`).
- `tests/qemu/test_mouse_selftest.sh` (new): types `mouse`, injects a
  pure-X synthetic movement (avoiding the Y-axis sign-inversion
  question entirely for a crisp, unambiguous assertion) and a button
  press/release, with real polling-based synchronization between steps
  (waiting for the guest's actual serial output, not a fixed guessed
  delay -- a timing bug in an EARLIER version of this test script,
  where a `mouse_button` was sent before the guest had finished
  processing the prior `mouse_move` read, produced one of the
  misleading intermediate failures during development). Run 4 times
  back to back with identical results after the real fix landed.
- `tests/qemu/test_shell_selftest.sh` and `test_timer_irq_selftest.sh`:
  both needed stale marker text updated (the `help` command list gained
  `mouse`; the PIC-init log line gained `/mouse` and `IRQ2+IRQ12`) --
  stale-marker fixes, not behavior regressions, the same pattern as
  every previous milestone's log-text change.
- All fifteen earlier smoke tests re-run and pass. All three host
  tests re-run and pass.
