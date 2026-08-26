#ifndef KERNEL_DRIVERS_ATA_H
#define KERNEL_DRIVERS_ATA_H

#include <stdbool.h>
#include <stdint.h>

/* Milestone 38 (ADR 0038): a polled-PIO ATA driver for the PRIMARY
   channel's SLAVE drive only -- the real disk this kernel's own build
   attaches deliberately at that exact, explicit position
   (`ide.0,unit=1` in the Makefile's own QEMU invocation) specifically
   so it can never collide with wherever `-cdrom`'s own boot media
   lands (QEMU's long-standing convention: secondary master,
   `ide.1,unit=0`), without needing to touch that proven, working boot
   path at all. Legacy/compatibility-mode fixed ports only (0x1F0-
   0x1F7, 0x3F6) -- the real PIIX3 IDE controller this kernel's own PCI
   scan already found (ADR 0013, bus 0/dev 1/fn 1, vendor 0x8086
   device 0x7010) operates in exactly this mode by default, needing no
   BAR reads at all, the same "well-precedented legacy path over a
   fancier one nothing here needs yet" bias ADR 0013's own Configuration
   Mechanism #1 choice and this codebase's PIC/PIT drivers already
   established.

   28-bit LBA only (max ~128GiB addressable -- far more than this
   hobby kernel's own scratch disk will ever need); polled PIO, not
   IRQ14-driven (matches the RTC driver's own busy-wait precedent for
   a similarly infrequent, non-hot-path operation -- unmasking/routing
   a 5th IRQ line is real, unneeded machinery for what this milestone
   actually needs).

   Register layout, status bits, command bytes, and the 400ns-delay-
   via-four-alternate-status-reads technique verified against the
   OSDev.org ATA PIO Mode wiki article -- the same class of source
   this codebase already treats as authoritative for legacy hardware
   register layouts (ADR 0005's PIC/PIT, ADR 0013's PCI config space).
   NOT extended to the primary channel's MASTER slot, the secondary
   channel, 48-bit LBA, or IRQ-driven transfers -- none of those are
   needed by anything this milestone actually builds; real,
   well-scoped future work if a later milestone ever does. */

#define ATA_SECTOR_SIZE 512u

/* Probes for the drive (the standard "read status, 0xFF means a
   floating bus, no drive attached" technique) and, if present, issues
   IDENTIFY DEVICE and records the addressable sector count. Safe to
   call even when no disk image is attached to this QEMU invocation --
   every OTHER existing smoke test boots with no disk at all, and this
   must never panic or hang for that ordinary case, only report it.
   Returns true if a drive was found and identified successfully. */
bool ata_init(void);

/* Whether ata_init() found a real drive -- callers (kernel_main's own
   self-test) use this to decide whether the read/write round-trip
   below is even meaningful to attempt this boot. */
bool ata_is_present(void);

/* Total addressable 512-byte sectors, from the drive's own IDENTIFY
   response -- 0 if no drive was found. */
uint64_t ata_sector_count(void);

/* Reads/writes exactly one 512-byte sector at 28-bit LBA `lba` into/
   from `buf` (the caller's own, already-owned 512-byte buffer -- no
   validation of a user-supplied pointer here, since this driver has
   no ring-3 caller yet; that validation belongs at whatever future
   syscall boundary exposes disk access to userspace, the same
   "validate at the actual boundary" stance CLAUDE.md's own security
   section already takes). Returns false if no drive is present, `lba`
   is out of range, or the drive reports an error (ERR/DF status bits)
   after the command -- never partially fills/reads `buf` on failure.
   ata_write_sector() issues CACHE FLUSH (0xE7) after the write and
   waits for it to complete before returning -- some real drives only
   commit a PIO write to the actual medium once flushed (a real,
   documented ATA PIO gotcha, not a guess), and a scratch disk this
   driver's own self-test immediately reads back from needs that
   write to be genuinely durable, not just buffered. */
bool ata_read_sector(uint64_t lba, uint8_t *buf);
bool ata_write_sector(uint64_t lba, const uint8_t *buf);

#endif /* KERNEL_DRIVERS_ATA_H */
