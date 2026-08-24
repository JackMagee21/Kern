# ADR 0013: PCI bus enumeration

## Status
Accepted and verified — `make run` boots the real ISO and enumerates
every real PCI function QEMU's default i440fx machine exposes (host
bridge, ISA bridge, IDE controller, PM bridge, VGA, and a NIC — 6
devices), confirmed against the actual, correctly-decoded vendor/
device/class IDs for each, not just a nonzero count. Landed correctly
on the first real boot, same as ADRs 0010-0012.

## Context
Fifth step of the post-Milestone-8 "build this into an OS" inventory —
"PCI enumeration," named in that original hardware/drivers list. This
kernel has no way to discover what hardware is actually present beyond
the handful of legacy, fixed-port devices already driven directly (PIC,
PIT, PS/2 keyboard, VGA text buffer, COM1 serial). Everything past that
— a real disk controller, a NIC, anything not on a fixed legacy port —
needs PCI enumeration first: there's no way to find a device's I/O
ports/MMIO BARs/IRQ line without reading its configuration space, and
no way to read that without first knowing which (bus, device, function)
it lives at.

## Decision
- **Legacy PCI "Configuration Mechanism #1"** (`CONFIG_ADDRESS`/
  `CONFIG_DATA`, I/O ports `0xCF8`/`0xCFC`) rather than the newer MMIO-
  based Enhanced Configuration Access Mechanism (which needs an ACPI
  MCFG table to even locate) — universally supported on any PCI-capable
  x86 system since the original PCI spec, no ACPI dependency, and this
  kernel doesn't parse ACPI tables at all yet (a listed CLAUDE.md
  non-goal pending explicit confirmation). Ports and the
  `CONFIG_ADDRESS` bit layout (bit 31 enable, bits 23-16 bus, bits 15-11
  device, bits 10-8 function, bits 7-2 dword-aligned register offset)
  verified against the OSDev.org PCI wiki article's documentation of
  the PCI Local Bus Specification's Configuration Mechanism #1 — the
  same class of source this codebase already treats as authoritative
  for legacy hardware register layouts (e.g. ADR 0005's PIC/PIT ports
  cross-checked against Linux's own driver source).
- **A brute-force scan of all 256 buses x 32 devices x 8 functions**,
  skipping straight to the next device when function 0's vendor ID
  reads back `0xFFFF` (the standard "nothing here" sentinel), and only
  probing functions 1-7 when function 0's header type has the
  multi-function bit (bit 7) set. This is the simple, well-precedented
  first approach (matching essentially every hobby-OS PCI tutorial);
  the more efficient bridge-recursive walk (skip whole unreachable
  buses via bridge topology) is deferred until enumeration speed or
  device count actually motivates it — nothing does yet.
- **Pure enumeration, no driver behavior**: `pci_scan()` reports what it
  finds via a callback and touches nothing else in a device's
  configuration space beyond the identifying fields (vendor/device ID,
  class/subclass/prog-if, header type). Deliberately stops short of
  actually driving any specific device (IDE, NIC, etc.) — that's
  separate, larger, per-device work for whenever a concrete disk or
  network need actually motivates it, not bundled into "can the kernel
  see what's there."
- **`libk/io.h` gained `outl`/`inl`** (32-bit port I/O) alongside the
  existing `outb`/`inb` — `CONFIG_ADDRESS`/`CONFIG_DATA` are 32-bit
  ports; this driver is the first thing in the kernel that's actually
  needed them.
- **Self-test asserts the Intel host bridge specifically** (vendor
  `0x8086`, bus 0/device 0/function 0) rather than just "found at least
  one device" — every QEMU i440fx-based machine (the default `-M pc`)
  has this exact device regardless of what other peripherals a
  particular QEMU version or invocation happens to expose (IDE/VGA/NIC
  presence isn't guaranteed the same way), so it's the one assertion
  that's actually stable across environments.

## Rejected alternatives
- **The MMIO-based Enhanced Configuration Access Mechanism** — would
  need ACPI MCFG table parsing just to find the MMIO base address,
  pulling ACPI support forward as a hidden dependency of a milestone
  that isn't supposed to need it; Configuration Mechanism #1 alone is
  sufficient for full bus enumeration on any real or virtual x86_64
  target this kernel currently targets.
- **A bridge-topology-aware recursive scan** (only descend into buses
  actually reachable through a discovered PCI-to-PCI bridge) instead of
  brute-forcing all 256 buses — more "correct" in the sense real OSes
  do this, but meaningfully more code for a benefit (faster enumeration
  on systems with many buses) this kernel doesn't need yet: a brute-
  force scan of 256*32*8 = 65536 possible slots, each skipped
  immediately on the two `inl`s needed to read a `0xFFFF` vendor ID,
  is fast enough in practice (confirmed: the self-test's boot-time
  budget is unaffected) to not be worth the added complexity now.
- **Starting to drive a specific device found by the scan** (e.g. the
  PIIX3 IDE controller, toward a future disk driver) — explicitly out
  of scope for this step; enumeration and driving a device are
  separable, and bundling them would make this milestone's scope
  depend on which specific device gets picked, when the actual
  near-term need (storage/filesystem) hasn't been decided yet.

## Verification
- `make run` (real toolchain) boots and prints, right after the `#BP`
  self-test: six `[PCI] bus ... dev ... fn ...: vendor ... device ...
  class ...` lines matching QEMU's actual i440fx devices (Intel host
  bridge `8086:1237`, PIIX3 ISA bridge `8086:7000`, PIIX3 IDE
  `8086:7010`, PIIX4 ACPI/PM `8086:7113`, a QEMU standard VGA
  `1234:1111`, and a NIC `8086:100e`), then `[OK] pci self-test passed
  (0x6 device(s) found, host bridge present)`. Every Milestone 1-12
  marker still prints unchanged afterward, through the shell prompt.
  Booted 4 times back to back with an identical device list and count
  each time — no flakiness, correct on the first real attempt.
- `tests/qemu/test_pci_selftest.sh` (new): asserts the exact host-
  bridge line (the one device guaranteed present regardless of QEMU
  version/config), the self-test's pass line, that the reported device
  count is at least 1, and that the shell still starts normally
  afterward.
- All twelve earlier smoke tests re-run and pass. All three host tests
  re-run and pass.
