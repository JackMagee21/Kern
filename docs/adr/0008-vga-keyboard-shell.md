# ADR 0008: VGA console, PS/2 keyboard, and an interactive shell

## Status
Accepted and verified (Milestone 8) — `make run` boots the real ISO,
prints on both serial and VGA, and real PS/2 keystrokes injected
through QEMU's monitor are read, echoed, and executed by the shell; see
Verification. Screenshots taken via QEMU's `screendump` confirm the
VGA output visually, not just via serial text.

## Context
Every milestone through 7 proved the kernel works, but only to someone
watching COM1 serial. Most real PCs — especially laptops, which is what
most people would actually test a hobby kernel on — have no physical
serial port, so the kernel would boot on real hardware and produce no
visible sign of life at all. This milestone is about making the kernel
observable and usable by someone sitting at the machine, not just in
QEMU with `-serial stdio`.

## Decision

- **Checked an assumption before proposing work: the existing GRUB2
  ISO already boots via both legacy BIOS and UEFI.** `xorriso
  -report_el_torito` on the built ISO showed two El Torito boot images
  (`BIOS` and `UEFI`), plus `/EFI/BOOT/BOOTX64.EFI` — `grub-mkrescue`
  auto-detected the installed `x86_64-efi` GRUB modules and built a
  proper hybrid image without any Makefile changes needed. Boot
  compatibility was not actually the gap; verified rather than assumed
  before deciding where to spend effort.
- **Legacy VGA text-mode console (0xB8000), not a Multiboot2
  framebuffer + bitmap font.** Simple (~100 lines: no font table, no
  pixel plotting, no scrolling-by-blit-of-pixels), and 0xB8000 is
  identity-mapped already (boot.asm's low 8MiB window) — no new
  mapping needed. Known, documented limitation: on a pure UEFI boot
  with no CSM, there is generally no live VGA text buffer at 0xB8000
  (no BIOS video service ever set one up), so this works reliably via
  legacy BIOS/CSM boot but not on UEFI-only paths. A framebuffer-based
  console would work uniformly on both, but is substantially more
  machinery for a first console — revisit if UEFI-only real hardware
  without CSM is actually the target.
- **A thin `console.c` fans every write out to both serial and VGA**,
  and `kernel_main`/`panic.c`/`exceptions.c` were migrated from calling
  `serial_write`/`serial_putc` directly to `console_write`/
  `console_putc`. This was a deliberate, if mechanical, refactor: every
  existing QEMU smoke test greps serial output, so keeping serial
  identical was necessary; but the entire point of this milestone is
  that a panic dump or a self-test result should be visible on a real
  screen too, not just on a serial port most real hardware doesn't
  have. Verified serial output is byte-identical before/after the
  migration (aside from expected register-value noise from the new
  VGA port I/O appearing in the `#BP` self-test's register dump).
- **Ring-3's `sys_write` also writes through `console_putc`** (was
  `serial_putc`), for the same reason — the whole point of adding a
  screen is that everything worth seeing should be visible on it,
  including output that originated in ring 3.
- **PS/2 keyboard, Scancode Set 1, US QWERTY, IRQ1.** Standard,
  long-stable hardware/layout facts (same confidence tier as the VGA
  CRTC cursor ports) — a wrong table entry makes one key produce the
  wrong character, not a silent triple fault, so this wasn't held to
  the GDT/IDT/page-table primary-source-fetch bar. Only make codes
  (press) are handled for the listed keys; function keys, arrows, and
  numpad are silently ignored — not needed for a line-oriented shell.
  Basic Left/Right Shift tracking gives real uppercase/symbol support
  without the complexity of Caps Lock, Ctrl/Alt combos, or non-US
  layouts.
- **The scancode→ASCII translation is a plain lookup table living in
  `keyboard.c` (kernel-only), but the queue between the IRQ handler and
  the shell's polling loop is `libk/ring_buffer.c` — pure, hardware-
  free logic, host-tested** (`tests/host/test_ring_buffer.c`: FIFO
  order, full-buffer drop without corrupting existing data, and
  wraparound past the physical end of the backing array). Same
  rationale as `libk/heap_alloc.c`: factor out what can genuinely be
  tested on the host, keep the hardware-only sliver (port 0x60, IRQ1
  registration) thin. `head`/`tail` are `volatile size_t` — the same
  "justified lock-free structure" reasoning as `pit.c`'s `tick_count`,
  documented as such rather than left implicit, since this is a queue
  (more moving parts than a counter) shared with an interrupt handler.
- **`keyboard.c` self-registers its own IRQ1 handler** (`keyboard_init`
  calls `irq_register_handler(1, ...)` directly), unlike `pit.c` since
  Milestone 6. No conflict exists here: nothing else needs to own IRQ1,
  so there's no reason to introduce the extra indirection Milestone 6's
  scheduler needed for IRQ0 specifically.
- **The shell is not a new task — it replaces `kernel_main`'s trailing
  idle loop.** `kernel_main` is already "task 0" (the bootstrap task) in
  the scheduler's round-robin; running `shell_run()` there means the
  shell is just one more fairly-scheduled participant alongside the
  Milestone 6/7 demo tasks, with no new scheduling concept needed.
  Waiting for keyboard input via `hlt` naturally yields the rest of its
  time slice each round, the same idle-wait pattern used everywhere
  else in this kernel.
- **Two tiny string-comparison helpers live directly in `shell.c`**
  rather than introducing a `libk/string.c` module. A 4-command shell
  needs exact-match and prefix-match on a handful of literal strings —
  writing that inline is smaller and clearer than a generic string
  library used by exactly one caller. Revisit once something else
  genuinely needs string utilities.
- **Four built-in commands**: `help`, `echo <text>`, `uptime` (prints
  `pit_get_ticks()`), `clear`. Chosen to be the smallest set that
  demonstrates real interactivity (output, argument handling, reading
  live kernel state, and a visible side effect) without inventing
  scope this milestone doesn't need (no scripting, no environment
  variables, no piping).

## Rejected alternatives
- **Multiboot2 framebuffer + bitmap-font console** — the more portable,
  UEFI-native-correct approach, rejected for *this* milestone on
  complexity grounds (font table, pixel plotting, scroll-by-blit) —
  see Decision. Worth building if UEFI-only hardware without CSM
  becomes the actual target.
- **A separate keyboard-input task** instead of folding the shell into
  `kernel_main`'s existing bootstrap-task slot — no benefit identified;
  would just be a second task doing the same round-robin-cooperative
  waiting the bootstrap task already does for free.
- **A generic `libk` string-comparison module** — deferred; exactly one
  caller needs exactly two small operations right now.

## Verification
- Checked before deciding scope: `xorriso -report_el_torito` on the
  built ISO confirmed working hybrid BIOS+UEFI boot already existed —
  avoided doing unneeded work based on an unverified assumption.
- `make run` (real toolchain) boots and prints all prior milestones'
  markers unchanged on serial, confirming the `console.c` fan-out
  refactor didn't alter serial output.
- **Visual verification, not just text**: booted headless with a QEMU
  monitor socket, used `screendump` to capture the actual VGA
  framebuffer as a PPM, converted to PNG (`zlib`+`struct`, no external
  image tools available), and read the image directly — confirmed the
  on-screen text exactly matches the serial log, including the full
  `#BP` fault dump and every self-test result.
- **Real keyboard input, not a shortcut**: used the QEMU monitor's
  `sendkey` command (virtual PS/2 controller, the same path real
  keystrokes take) to type `help` and `echo hello world` into a live
  boot; both were correctly read, echoed, and executed, confirmed by a
  second screenshot matching the serial transcript exactly.
- `tests/host/test_ring_buffer.c`: genuinely host-compiled and run
  (`gcc -fsanitize=address,undefined`), 4 test functions covering
  empty-buffer behavior, FIFO order, full-buffer drop without
  corruption, and wraparound. All pass.
- `tests/qemu/test_shell_selftest.sh` (new): automates the same
  `sendkey`-based verification — waits for the real shell-prompt
  marker in serial output before sending keys (not a fixed guessed
  delay, since the PS/2 controller's output buffer holds only one byte
  and keys sent before the guest is polling can be lost), types `help`
  and `echo shelltest123`, and asserts the exact expected output
  appears. Run three times back to back with no flakiness observed.
- All seven earlier milestones' smoke tests re-run and pass
  (`test_timer_irq_selftest.sh` needed its expected marker string
  updated after `kernel_main`'s log line changed to mention the new
  keyboard init — a test-text fix, not a behavior regression, same
  pattern as Milestone 6→7). All three host tests
  (`test_fmt`, `test_heap_alloc`, `test_ring_buffer`) pass.
