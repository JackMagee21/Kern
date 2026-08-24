# ADR 0014: CMOS RTC driver and a `date` shell command

## Status
Accepted and verified — `make run` boots the real ISO, reads the CMOS
RTC, and decodes a wall-clock time matching the actual host date at
boot; the shell's new `date` command was exercised with real injected
keystrokes and produced the expected format. Landed correctly on the
first real boot, same as ADRs 0010-0013.

## Context
Sixth step of the post-Milestone-8 "build this into an OS" inventory —
"RTC," named in the original hardware/drivers list, and one of the
few remaining items that doesn't touch a CLAUDE.md non-goal (unlike
storage/filesystem or ACPI-based shutdown, both flagged to the user
rather than started). A kernel with no notion of wall-clock time can't
timestamp anything or show a user what time it is — `uptime` only ever
reports ticks since boot, not an actual date.

## Decision
- **MC146818-compatible CMOS RTC, ports `0x70`/`0x71`** (`CONFIG_
  ADDRESS`-style select-then-read, not memory-mapped) — the standard,
  decades-stable legacy interface present on every PC-compatible
  target this kernel runs on (real or QEMU). Register indices and
  status-register bit layout (Status A's UIP bit, Status B's binary/
  BCD and 12/24-hour mode bits) verified against the OSDev.org "CMOS"
  wiki article's documentation of this hardware — the same class of
  source this codebase already treats as authoritative for legacy
  register layouts (ADR 0005's PIC/PIT, ADR 0013's PCI config space).
- **Double-read-until-stable, not an IRQ8-driven update-ended
  interrupt.** Reads twice (waiting for Status A's UIP bit to clear
  before each), retrying until both reads agree — the standard way to
  avoid a value torn by the RTC's own ~244us update cycle (e.g. seconds
  already rolled over while hour/minute were read from before the
  rollover). An IRQ8-based scheme would need another PIC line unmasked
  and another interrupt handler for a feature that's read once at boot
  and occasionally from the shell — not worth the added surface for
  this milestone's actual usage pattern.
- **Always returns 24-hour binary fields, regardless of the hardware's
  configured mode** — `rtc_read()` does the BCD->binary and 12->24-hour
  conversion internally (checking Status B rather than assuming either
  mode), so every caller works with plain numbers and never needs to
  know or check how the RTC happened to be configured.
- **2-digit year, no century register read**: `RTC_REG_YEAR` is a
  2-digit BCD/binary value with no standard century register in the
  base MC146818 register set this driver targets (some systems expose
  one at a model-specific CMOS offset, deliberately not relied on
  here). Assumes the 21st century, correct for any date this kernel
  will realistically run on and consistent with what most minimal RTC
  drivers do.
- **`libk/fmt.c` gained `u32_to_dec()`** (decimal, unpadded) — the
  first decimal formatter in a kernel that previously only had hex
  (`u64_to_hex`), needed because a date/time genuinely reads better in
  decimal than hex to a human looking at the shell. Host-tested
  alongside the existing hex formatter, same file, same pattern.
  2-digit zero-padding for the `date` command's `HH:MM:SS`/`MM-DD`
  fields stays a small shell-local helper (`write_2digit`) rather than
  a second libk function — padding is a display concern specific to
  this one command, not a general-purpose primitive worth generalizing
  yet.
- **The boot-time self-test checks range validity, not an exact
  expected value** — there's no way to know the "correct" wall-clock
  time a test will run at, unlike every other self-test in this kernel
  which checks a specific, deterministic outcome. Checking
  `0 <= seconds <= 59`, `1 <= day <= 31`, `2020 <= year <= 2100`, etc.
  is still a real, meaningful correctness check: a BCD-vs-binary
  decoding bug or a wrong register index would very likely produce an
  out-of-range value in at least one field (e.g. a raw BCD `0x59`
  misread as binary 89), not silently look plausible.

## Rejected alternatives
- **An IRQ8 update-ended interrupt to detect a safe read window**,
  instead of the busy-wait/double-read pattern — more "proper" in the
  sense real OSes with a live clock subsystem use this, but adds a new
  interrupt line and handler for a feature this kernel only reads
  occasionally (boot, and on-demand from the shell), not continuously;
  deferred until something actually needs interrupt-driven timekeeping
  (e.g. a real `date -s` / settable system clock, or scheduling by
  wall-clock time).
- **A CMOS century-register read** for a fully general 4-digit year —
  rejected: the century register's CMOS offset isn't part of the
  standard MC146818 set and varies by chipset/BIOS; assuming the 21st
  century is simpler, correct for this kernel's realistic lifetime, and
  avoids depending on a non-standardized, harder-to-verify detail.
- **Baking 2-digit zero-padding into `u32_to_dec()` itself** (e.g. an
  optional width parameter) — rejected as premature generalization;
  the one place that needs padding (`date`'s display format) is small
  enough to keep local, matching CLAUDE.md's "three similar lines is
  better than a premature abstraction."

## Verification
- `make run` (real toolchain) boots and prints, right after the PCI
  self-test: `[OK] rtc self-test passed, boot time (fields in hex...)
  year 0x7ea month 0x8 day 0x18 hour 0xd min 0x11 sec 0x1c` — decoding
  to 2026-08-24 13:17:28, matching the actual date the boot ran on.
  Every Milestone 1-13 marker still prints unchanged afterward, through
  the shell prompt. Booted 4 times back to back with a correctly-
  advancing time each run — no flakiness, correct on the first real
  attempt.
- The shell's new `date` command was exercised with REAL injected
  keystrokes (QEMU monitor `sendkey`, the same mechanism ADR 0008's
  shell test already established, not a shortcut) via an extension to
  `tests/qemu/test_shell_selftest.sh`: typed `date`, confirmed the
  output ends in `UTC (from CMOS RTC)` in the expected `YYYY-MM-DD
  HH:MM:SS` format. That test's stale `help` text assertion (missing
  the new `date` command) was also fixed.
- `tests/qemu/test_rtc_selftest.sh` (new): asserts the boot self-test's
  marker and independently re-extracts and range-checks the decoded
  year from the real captured output, not just trusting the marker's
  presence.
- `tests/host/test_fmt.c` (extended): six new `u32_to_dec()` checks,
  including the zero/single-digit/multi-digit boundary cases and
  `UINT32_MAX`.
- All thirteen earlier smoke tests re-run and pass. All three host
  tests re-run and pass.
