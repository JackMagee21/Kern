# ADR 0015: Legacy (non-ACPI) reboot

## Status
Accepted and verified — `make run` boots the real ISO, the shell's new
`reboot` command triggers a genuine CPU reset (confirmed by watching
the full boot sequence print a second time when QEMU is allowed to
actually restart the VM, and separately by QEMU exiting promptly under
`-no-reboot` when it isn't). Landed correctly on the first real boot;
one real test-infrastructure bug was found and fixed along the way
(`-no-shutdown` overriding `-no-reboot`'s exit behavior) — see
Verification.

## Context
Seventh step of the post-Milestone-8 "build this into an OS" inventory
— "shutdown/reboot," named in the original hardware/drivers list.
Proper ACPI-based power-off needs ACPI table parsing (a listed
CLAUDE.md non-goal, flagged to the user rather than started after
Milestone 13). A full system RESET, however, doesn't need ACPI at all
— it's a decades-older mechanism BIOSes relied on long before ACPI
existed, and it's the one piece of "shutdown/reboot" this kernel can
implement right now without touching the flagged non-goal.

## Decision
- **The legacy 8042 keyboard controller's "pulse output line 0"
  command** (port `0x64`, command byte `0xFE`) as the primary
  mechanism — the standard non-ACPI reset every PC-compatible BIOS/OS
  has used historically (the same one behind the classic Ctrl-Alt-Del
  reset path). Port, status bit, and command byte verified against the
  OSDev.org "Reboot" wiki article's documentation of this mechanism —
  the same class of source this codebase already treats as
  authoritative for legacy hardware (ADR 0005, ADR 0013, ADR 0014).
- **An intentional triple-fault fallback** if the controller doesn't
  respond: load a deliberately invalid IDTR (limit 0), then trigger an
  exception (`int3`) that can't be handled, which the CPU resolves by
  resetting itself (Intel SDM Vol. 3A Sec. 6.15's fault escalation:
  unhandleable exception -> `#GP` -> `#DF` -> triple fault -> reset).
  Unconditional and hardware-assumption-free — it doesn't matter
  whether a keyboard controller is even present.
- **Lives in `kernel/arch/x86_64/reboot.c/.h`**, not `drivers/
  keyboard.c` — despite using the same physical 8042 controller,
  `reboot()` is a CPU/system-level operation (a synchronous, one-shot
  reset request), not keyboard input handling; keeping it separate
  matches how `tss.c`/`syscall.c` stay separate arch-level concerns
  from the drivers they're adjacent to.
- **A `reboot` shell command** — the same "make the kernel usable, not
  just correct" motivation as every other shell command (ADR 0008's
  `help`/`echo`/`uptime`/`clear`, ADR 0014's `date`).

## Rejected alternatives
- **ACPI-based reset/shutdown** (writing the FADT's `PM1a_CNT` reset/
  sleep register) — needs ACPI table parsing just to locate that
  register, a listed non-goal pending confirmation; out of scope here
  by design, not an oversight.
- **QEMU-specific debug-exit device** (`isa-debug-exit`) — not a real
  hardware mechanism, wouldn't do anything on real hardware or a QEMU
  invocation that doesn't specifically add that device; rejected as
  not a genuine "reboot" capability, just a QEMU testing convenience.
- **Relying solely on the triple-fault fallback**, skipping the
  keyboard-controller path entirely — rejected: the controller path is
  the "clean" mechanism real BIOSes use and is expected to work on any
  real target; the triple fault is deliberately kept as a fallback
  specifically for the rare case the controller doesn't respond, not
  promoted to the primary mechanism.

## Verification
- `make run` (real toolchain), booted WITHOUT `-no-reboot` (allowing an
  actual VM restart): typed `reboot` through real injected keystrokes
  (QEMU monitor `sendkey`), and the ENTIRE Milestone 1-14 boot sequence
  printed a second time from `[OK] hello kernel` onward — direct proof
  the CPU genuinely reset, not just that `reboot()` was called and
  didn't crash.
- **A real test-infrastructure bug found and fixed while verifying
  this**: the first attempt at a QEMU smoke test used this suite's
  usual `-no-reboot -no-shutdown` combination and observed QEMU
  neither reboot nor exit -- it just hung until the test's own
  timeout. Diagnosed by testing the flag combinations by hand: with
  `-no-reboot` alone, QEMU exits promptly (~1-3s) after the reset;
  adding `-no-shutdown` back causes it to hang instead. `-no-shutdown`
  (meant for post-mortem debugging via the monitor after a
  shutdown/reset) overrides `-no-reboot`'s own "exit instead of
  rebooting" behavior -- undocumented interaction, not guessed, found
  by isolating each flag's effect individually before writing the
  final test. `tests/qemu/test_reboot_selftest.sh` deliberately omits
  `-no-shutdown`, with a comment explaining why it's the one test in
  this suite that does.
- `tests/qemu/test_reboot_selftest.sh` (new): types `reboot` through
  real injected keystrokes, then waits for QEMU to exit ON ITS OWN
  (never force-killed until the wait returns) and asserts it did so
  well before the timeout -- a kernel that ignored the reboot command
  would instead run until the full timeout, clearly distinguishable.
- `tests/qemu/test_shell_selftest.sh`: needed its `help`-text assertion
  updated for the new `reboot` command (stale-marker fix, not a
  behavior regression, the same pattern as every previous milestone's
  help-text change).
- All fourteen earlier smoke tests re-run and pass. All three host
  tests re-run and pass.
