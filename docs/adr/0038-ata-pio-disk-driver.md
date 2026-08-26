# ADR 0038: a real ATA PIO disk driver

## Status
Accepted and verified — `kernel/drivers/ata.c` reads and writes real
512-byte sectors on a genuine attached disk via polled PIO on the
primary channel's slave drive, the first step of the filesystem arc
the user explicitly authorized (2026-08-26: "go ahead with the
filesystem work"). Booted clean on the FIRST real attempt (both with
and without a disk attached). A real, serious, PRE-EXISTING
correctness bug was found and fixed during KVM verification — see
Decision's last bullet; not related to the disk driver itself, but
found only because this milestone's own verification pass finally
exercised repeat real-KVM boots thoroughly enough to expose it. All
thirty-two QEMU smoke tests and all four host suites pass.

## Context
`future.md`'s own "Explicitly flagged" section had listed "a disk
driver + real filesystem" as the one CLAUDE.md non-goal item, noting
Milestone 13's PCI scan had already found a real PIIX3 IDE controller
(bus 0/dev 1/fn 1, vendor 0x8086 device 0x7010) in QEMU's default
machine. With the user's explicit go-ahead, this milestone builds the
actual disk-access layer everything else in the filesystem arc will
sit on top of — deliberately scoped to ONLY the disk driver itself
(sector read/write), not a filesystem format on top of it, matching
this codebase's own "hardest-unknown-first, one subsystem per change"
roadmap sequencing.

## Decision

- **Polled PIO, legacy/compatibility-mode fixed ports (0x1F0-0x1F7,
  0x3F6), primary channel, SLAVE drive only.** The real PIIX3
  controller already found operates in exactly this mode by default —
  no BAR reads needed, matching this codebase's own established bias
  toward the well-precedented legacy path (ADR 0013's Configuration
  Mechanism #1 over the ACPI-dependent alternative, this kernel's own
  PIC/PIT drivers over IOAPIC/HPET). Not IRQ14-driven — matches the
  RTC driver's own busy-wait precedent for an infrequent, non-hot-path
  operation; unmasking and routing a 5th IRQ line is real, unneeded
  machinery this milestone doesn't need. 28-bit LBA only (~128GiB
  ceiling, far more than this hobby kernel's scratch disk will ever
  need).
- **Register layout, status bits, command bytes, the 400ns-delay
  technique, and the IDENTIFY DEVICE sector-count field offset (words
  60-61) all verified against the OSDev.org ATA PIO Mode wiki article**
  (fetched directly, not recalled from memory — CLAUDE.md's "never
  guess hardware/ABI facts... cross-check against
  authoritative documentation" applied to legacy ATA the same way
  ADR 0013 already applied it to PCI config space and ADR 0005 to
  PIC/PIT ports).
- **A NEW, explicitly-placed raw disk image (`build/disk.img`, 16MiB,
  zero-filled via `dd`), attached at the EXPLICIT slot `ide.0,unit=1`
  (primary slave) — deliberately never touching the proven, working
  `-cdrom` boot-media path at all.** QEMU's own long-standing
  convention places `-cdrom`'s media at `ide.1,unit=0` (secondary
  master); explicit `-device ide-hd,...,bus=ide.0,unit=1` placement for
  the new disk guarantees zero collision regardless of QEMU version
  defaults, verified by an actual boot rather than assumed. Wired into
  `run`/`debug` (a real interactive session gets a disk too) but NOT
  into any OTHER existing smoke test — none of them need one, and the
  driver's own graceful "no drive" handling (below) is exactly what
  makes that safe.
- **Floating-bus detection (0xFF status read) as the FIRST thing
  `ata_init()` does, with NO panic if no drive is found.** Every other
  existing smoke test boots with no disk image attached at all — that
  MUST stay the ordinary, ungraded case, not a failure. `ata_init()`
  returns false and `kernel_main`'s own self-test prints an
  informational (not `[FAIL]`) marker in that case, exactly matching
  what every pre-existing test's own boot log already looks like.
- **`CACHE FLUSH` (0xE7) issued and waited out after every write**,
  before `ata_write_sector()` returns success. A documented real ATA
  PIO gotcha (some drives only commit a PIO write to the actual medium
  once explicitly flushed) — this driver's own self-test immediately
  reads back what it just wrote, so a buffered-but-not-yet-durable
  write would be a real, if narrow, correctness gap for exactly the
  scenario this milestone needs to prove.
- **A real, serious, PRE-EXISTING race condition found and fixed
  during verification — not part of the disk driver itself, but found
  because this milestone's own KVM verification pass was the first to
  run several REPEAT real-KVM boots specifically while chasing down an
  unrelated frame-leak panic.** `display_server.c`'s own boot-time
  setup loop (serving clients A/B/pulse/clock in strict sequence) had
  ALWAYS trusted that the very next message in its own inbox was
  exactly the `DISPLAY_OP_REQUEST`/`DISPLAY_OP_PRESENT` it was waiting
  for — true only as long as nothing else could message the server
  during that setup window. That stopped being safe the moment the
  pulse app (Milestone 33) became a PERSISTENT client that starts
  sending its own background `DISPLAY_OP_REDRAW` pings the instant its
  own handshake completes, potentially WHILE a LATER client (the clock
  app, Milestone 35) is still completing its own. TCG's much slower
  execution never exposed this (pulse app's own 200000-`sys_nop` pacing
  spin takes long enough, in real wall-clock terms, under software
  emulation, that the whole 4-window boot sequence always finished
  first) — real KVM's hardware-accelerated speed genuinely can and did
  trigger it on an actual boot: a stray `DISPLAY_OP_REDRAW` got misread
  as the clock app's own `PRESENT`, and the clock app's OWN subsequent
  genuine `REQUEST` message got misread as that `PRESENT`'s `shm_id`
  field (190 -- exactly the clock app's own `REQUESTED_W`), causing
  `shm_map()` to fail, the server to exit its own boot loop with
  `return 1`, and a cascading process-lifecycle frame-count mismatch.
  Root-caused with hard evidence (a full message-flow trace added
  temporarily to `kernel/ipc/msgqueue.c`, showing the exact
  misinterpreted message field-for-field), not guessed. Fixed using
  the EXACT SAME technique Milestone 36's own `handle_dynamic_request()`
  already established for this class of problem: two new helpers,
  `recv_boot_request()`/`recv_boot_present()`, that keep processing
  anything that ISN'T the specific expected message (opcode, and for
  `PRESENT`, sender pid too) via the existing `dispatch_message()`,
  instead of trusting the next message blindly. Verified with 5+ repeat
  real-KVM boots (previously 100% reproducible) showing zero failures
  afterward.

## Rejected alternatives
- **IRQ14-driven transfers** instead of polled PIO. Rejected — matches
  the RTC driver's own precedent (a real, if less common, choice for an
  infrequent operation); routing and unmasking a 5th IRQ line is real
  machinery this milestone's actual need (a working read/write round
  trip, proven once) doesn't call for.
- **48-bit LBA / the secondary channel / the primary channel's master
  slot.** Rejected as unneeded scope — 28-bit LBA's ~128GiB ceiling is
  far beyond this hobby kernel's own scratch disk, and the primary
  slave slot was chosen SPECIFICALLY to guarantee zero collision with
  `-cdrom`'s own conventional placement without touching that proven
  boot path at all.
- **Reaching for ATAPI support** to read the actual `-cdrom` boot media
  instead of a separate raw disk image. Rejected — ATAPI's packet
  command protocol is a genuinely different, more complex interface
  than plain ATA PIO, and ISO9660 (the CD's own filesystem) is
  read-only; a plain writable raw disk is what every later filesystem
  milestone in this arc will actually need.
- **Fixing the display-server race by adding a delay, retry loop, or
  reordering the go-signal chain** rather than validating the received
  message. Rejected — those would treat the SYMPTOM (bad timing) rather
  than the actual bug (an unvalidated assumption about message
  ordering); the fix applied instead is provably correct regardless of
  timing, the same standard ADR 0032's own SS-corruption fix and
  Milestone 36's own `dispatch_message()` pattern already set.

## Verification
- `make run`/a real headless boot with NO disk attached: `[OK] ata: no
  drive detected (expected unless build/disk.img is explicitly
  attached)`, no panic, identical to every pre-existing smoke test's
  own unaffected boot sequence.
- A real headless boot WITH `build/disk.img` (16MiB, `dd`-zeroed)
  explicitly attached at `ide.0,unit=1`: `IDENTIFY` correctly reports
  exactly 0x8000 (32768) sectors = 0x1000000 (16777216) bytes, matching
  the disk image's own real size; a write of a real, non-constant
  pattern to LBA 2000 followed by a read-back verified byte-for-byte
  across all 512 bytes. Passed on the FIRST real attempt.
- **`tests/qemu/test_ata_selftest.sh` (new)**: explicitly attaches
  `build/disk.img`, boots headless, asserts the exact sector-count and
  round-trip markers, and confirms neither the driver's own failure
  path nor a false "no drive" report fired.
- Booted 3+ additional repeat times under real KVM acceleration WITH
  the disk attached, zero faults, identical markers every time.
- The display-server race: reproduced 100% (5/5) under real KVM before
  the fix; 5/5 clean after, with a full message-flow trace confirming
  the exact corrupted field values before the fix and their absence
  after.
- All thirty-two QEMU smoke tests (thirty-one pre-existing, one new)
  and all four host test suites re-verified passing on a clean
  rebuild — every pre-existing test boots with no disk attached, proving
  the driver's own "no drive" path is genuinely harmless, not just
  reasoned to be.

## Known limitations (accepted for this milestone only)
Primary-slave-only — no secondary channel, no master-slot support, no
IRQ-driven transfers, no 48-bit LBA. No partition table / MBR parsing
(sector 2000 was chosen specifically clear of where a future
filesystem's own boot sector/superblock will eventually live, but
nothing currently reserves or formats that region). No error-recovery
beyond a single attempt per operation (a transient failure is reported
to the caller, not retried). This driver has no ring-3 syscall
exposure yet — it's kernel-only, proven only by `kernel_main`'s own
boot self-test; exposing real disk access to userspace (with the same
"validate a user-supplied pointer/length at the actual syscall
boundary" discipline CLAUDE.md's security section already requires) is
real future work once an actual filesystem format sits on top of this.
