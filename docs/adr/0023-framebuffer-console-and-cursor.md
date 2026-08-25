# ADR 0023: Graphics framebuffer console and mouse cursor

## Status
Accepted and verified -- `make run` boots the real ISO; a QEMU
`screendump` taken at the shell prompt, visually inspected, shows every
boot-log line rendered as CORRECT, legible text (not mirrored/garbled --
the font's bit-order convention was right on the first attempt, verified
by looking at the actual rendered pixels rather than trusted from
memory) and a small red mouse-cursor square at the exact expected
center position. A second screendump taken after injecting a real
synthetic PS/2 `mouse_move 100 50` via the QEMU monitor shows the cursor
at the EXACT expected new pixel position, with zero leftover pixels at
its old position (erase-before-redraw genuinely restores what was
underneath, not just draws a second cursor). `-d int,cpu_reset` trace
across a full boot: unchanged from Milestone 22 (still exactly 1 `#BP`
+ 3 `#PF`, zero double-fault/reset) -- this milestone introduced zero
new fault-driven code paths. All twenty-two pre-existing smoke tests
(including `test_mouse_selftest.sh` and `test_shell_selftest.sh`, both
directly touched by this milestone's design) and all four host test
suites re-verified passing. Correct on the first real boot attempt;
booted 5 times back to back with identical shape every time.

## Context
Milestone 16 built a real PS/2 mouse driver with nothing to draw a
cursor on -- VGA text mode (ADR 0008) has no pixels. `future.md` flagged
"cursor/graphics" as the natural next reasonable-next-step. Before
writing any code, a real design fork was found and explicitly checked
with the user rather than guessed past: satisfying a Multiboot2
framebuffer request switches the actual video HARDWARE mode, so
Milestone 8's VGA text console (`0xB8000`) and a linear graphics
framebuffer cannot be active simultaneously -- drawing a cursor on a
pixel framebuffer would otherwise silently regress the existing
on-screen shell. The user chose the full-replacement option: build real
text rendering on the framebuffer (closing ADR 0008's own flagged
UEFI-without-CSM gap as a side effect) rather than a narrower opt-in
graphics-mode demo.

## Decision

- **Multiboot2 framebuffer request tag, verified against the canonical
  GRUB header before writing any code** (`rhboot/grub2`'s
  `multiboot2.h`, the same primary source ADR 0001 already used) --
  `kernel/arch/x86_64/boot.asm` requests 1024x768x32, flagged
  `MULTIBOOT_HEADER_TAG_OPTIONAL` (so an unsatisfiable request doesn't
  hard-fail the boot at the bootloader level; `fb_init()` panics with a
  clear kernel-side message instead if no tag comes back, a better
  failure mode than an opaque GRUB error). **Tested incrementally before
  building anything on top, per CLAUDE.md's boot-protocol discipline**:
  a temporary probe confirmed GRUB honored the request exactly (observed
  `addr=0xfd000000 pitch=0x1000 width=0x400 height=0x300 bpp=0x20
  type=0x1`) with ZERO `grub.cfg` changes needed -- `grub-mkrescue`'s
  default module set already includes what's required. This was
  verified empirically, not assumed, before the probe was removed and
  the real driver built.
- **The framebuffer's physical memory is reached via the EXISTING
  Milestone 19 direct-map (`vmm_phys_to_virt()`), not a new dedicated
  mapping.** QEMU's negotiated framebuffer BAR (`0xfd000000`) sits well
  under 4GiB, already covered unconditionally by the direct-map's own
  0..4GiB 2MiB-page mapping -- `kernel/drivers/framebuffer.c` needed
  ZERO new page-table work, just a defensive panic if a future/different
  target's framebuffer address ever fell outside that window. No MTRR/
  PAT tuning for the MMIO region (write-back cacheable, the same
  attribute every other direct-mapped page gets) -- accepted as
  out-of-scope the same way ACPI-adjacent perf tuning already is
  (CLAUDE.md non-goals).
- **`fb_ready`, a module-level guard checked FIRST by every
  `framebuffer.c` mutator (and mirrored by `fbconsole.c`'s own
  `cols == 0` check).** `fb_init()` can only run once
  `vmm_phys_to_virt()` is usable -- itself only available partway
  through `kernel_main`, after `pmm_init()`/`vmm_direct_map_init()` --
  meaningfully LATER than Milestone 8's old `vga_init()`, which ran at
  time zero. Every `console_write()` call before that point still
  reaches serial (`console.c`'s existing fan-out, unchanged); the
  graphics half is a silent, deliberate no-op until `fb_init()` sets
  `fb_ready`, specifically so a panic/fault dump that fires before then
  can never itself crash by touching an uninitialized framebuffer --
  CLAUDE.md's "never fail silently on the panic path" cuts the other
  way here: a panic that itself faults would hide the real error, which
  is strictly worse than a few early boot lines only reaching serial.
- **`kernel/drivers/font8x8.h`: a real, well-known public-domain 8x8
  bitmap font (printable ASCII 0x20-0x7E, 95 glyphs), fetched and
  byte-for-byte extracted from `github.com/dhepper/font8x8`'s
  `font8x8_basic.h` -- NOT hand-authored/guessed pixel art.** Applying
  CLAUDE.md's "never guess... when precision matters" discipline to a
  visual-correctness concern rather than a hardware/ABI one: a
  first-try naive regex extraction actually DID corrupt one row (the
  `U+007B` glyph's own trailing comment, `({)`, contains a literal `{`
  that a naive non-line-anchored regex mistook for the start of the
  NEXT row's byte array, corrupting `U+007C`'s data) -- caught by
  validating every extracted row is exactly 8 well-formed `0xXX` tokens
  before ever writing the embedded header, not discovered by a garbled
  on-screen glyph. Fixed by anchoring the extraction regex to line
  starts. The font's bit-order convention (LSB = leftmost pixel) was
  ultimately confirmed correct the way a cosmetic-only concern should
  be -- rendered and LOOKED AT via a QEMU screendump -- rather than
  guessed and shipped unverified.
- **`kernel/drivers/fbconsole.c`: same `putc`/`clear`/`write` interface
  shape `vga.c` had**, so `console.c`'s fan-out needed only a one-line
  swap. Cell grid dimensions are computed at `fbconsole_init()` time
  from the ACTUAL negotiated `fb_get_width()`/`fb_get_height()` (128x96
  cells at 1024x768/8x8), never hardcoded to the old 80x25. Scrolling is
  a raw pixel-row memory copy (`fb_scroll_up()`, new in
  `framebuffer.c`) rather than composed of many bounds-checked
  single-pixel writes -- the framebuffer-memory equivalent of `vga.c`'s
  own per-cell `scroll()`.
- **`kernel/drivers/cursor.c`: a save/restore ("sprite") pattern** --
  `fb_read_rect()` (new) captures the 8x8 pixel block under the cursor's
  CURRENT position before it moves, `fb_fill_rect()` draws the cursor;
  moving it erases (restores the saved block) at the OLD position first,
  THEN updates the tracked position, THEN saves+draws at the NEW one --
  ordering that matters and was worked out on paper before writing code
  (an erase-after-updating-position bug would have restored the wrong
  pixels). PS/2's own `dy` sign convention (positive = up, `mouse.h`'s
  Milestone 16 doc comment) is negated exactly ONCE, here -- the "future
  display consumer decides its own sign convention" that same doc
  comment anticipated.
- **A genuinely NEW problem found and designed around BEFORE it ever
  broke a test: two independent consumers of the same mouse hardware
  stream.** `cursor_poll()` (called continuously from `shell.c`'s
  `read_line()` wait loop) and the shell's existing `mouse` command
  (`shell.c`, unchanged since Milestone 16) both need to observe every
  decoded packet -- a SINGLE shared queue would let whichever one polls
  first silently steal the other's event, which would have broken
  `test_mouse_selftest.sh`'s already-passing, already-tested contract
  (its `mouse` command call expects to find the event ITS OWN
  `mouse_move` injection produced, not nothing because `cursor_poll()`
  already drained it). Fixed by giving `mouse.c` a SECOND, independent
  ring buffer (`mouse_has_cursor_event()`/`mouse_get_cursor_event()`),
  fed by the same `queue_push()` broadcast as the existing debug queue
  -- reasoned through by reading `shell.c`'s existing `mouse` command
  code before writing `cursor.c`, not discovered by a live test
  failure.
- **`kernel/drivers/vga.c`/`vga.h`: retired (deleted), not left dormant.**
  Once GRUB switches video hardware into linear-framebuffer mode to
  satisfy the Multiboot2 tag, `0xB8000` is no longer live VGA text-mode
  memory -- keeping the driver around unused-but-present would be
  actively misleading (or actively wrong, if some future change
  accidentally called it), the same "certain it's unused -> delete
  completely" stance CLAUDE.md already establishes, and the same
  precedent Milestone 17 set retiring `user_demo.asm` when superseded.
- **`kernel/arch/x86_64/multiboot2.c`: a new shared `multiboot2_find_tag()`
  helper**, factored out once `framebuffer.c` became a SECOND real
  consumer of the same tag-list-walking logic `pmm.c` already had inline
  (CLAUDE.md's "reuse once genuinely duplicated, not preemptively"
  discipline) -- `pmm.c`'s own MMAP-tag lookup refactored to use it too,
  a pure behavior-preserving change re-verified by every pmm-touching
  test still passing unmodified.

## Rejected alternatives
- **A dedicated new page-table mapping for the framebuffer** (a fresh
  `PDPT` slot, `vmm_map_page()` calls at boot) instead of reusing the
  direct-map. Rejected once the framebuffer's actual negotiated address
  (`0xfd000000`) was confirmed to already fall within the direct-map's
  existing 4GiB coverage -- building a second, parallel MMIO-mapping
  mechanism for a case the general one already handles would duplicate
  machinery for no benefit.
- **A single shared mouse-event queue**, with `cursor_poll()` reusing
  the existing `mouse_get_event()` debug API. Rejected -- see Decision;
  would have broken `test_mouse_selftest.sh`'s existing contract.
- **Hand-authoring bitmap font glyphs from memory.** Rejected outright
  as exactly the kind of "guess when precision matters" CLAUDE.md warns
  against, even though a font glyph being visually wrong wouldn't crash
  anything -- fetching a real, attributed, public-domain source and
  verifying every extracted byte was strictly cheaper and more reliable
  than getting 95 glyphs' pixel art right from memory on the first try.
- **Keeping `vga.c` present but unused, "just in case."** Rejected --
  see Decision; it would be actively wrong to run (writes to memory
  that's no longer a text buffer once graphics mode is active), so
  keeping it around serves no purpose CLAUDE.md's "delete what's
  certainly unused" doesn't already cover.
- **A blinking text-input caret on the framebuffer console**, matching
  VGA text mode's hardware cursor. Rejected as out of this milestone's
  actual scope (cursor/graphics meant the MOUSE cursor, per `future.md`'s
  own framing) -- the shell's own `> ` prompt and character echo already
  make the input position visually obvious without one; flagged as a
  Known limitation, not silently dropped.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-22
  marker unchanged, `[OK] hello kernel` through `[OK] direct-map
  self-test passed` reaching serial only (as documented, before
  `fb_init()` can run), then (new) `[OK] graphics framebuffer console
  initialized` -- every subsequent message reaches both serial and the
  screen, exactly as `console_write()`'s existing contract always
  promised.
- A QEMU `screendump` at the shell prompt, visually inspected (not
  merely "a file was produced"): every line of boot-log text is
  correctly formed and legible, proving the font's bit-order convention
  was right without needing to trust it from memory.
- `-d int,cpu_reset` trace across a full boot: unchanged from Milestone
  22 (1 `#BP`, 3 `#PF`, zero double-fault/reset) -- this milestone
  introduced no new exception-driven code paths at all.
- `tests/qemu/test_framebuffer_selftest.sh` (new): takes two real
  screendumps around a real injected `mouse_move 100 50` (QEMU monitor,
  actual virtual PS/2 hardware) and asserts the cursor's 8x8 red block
  is found at the EXACT expected pixel bounding box both before AND
  after -- not just "the cursor exists somewhere" -- and that the AFTER
  dump has no leftover red pixels at the OLD position (proves
  erase-before-redraw, not just draw-a-second-cursor). All twenty-two
  earlier smoke tests (`test_mouse_selftest.sh` and
  `test_shell_selftest.sh` specifically re-verified, since both were
  directly touched by this milestone's design) and all four host test
  suites re-verified passing with no assertion changes needed. Booted 5
  times back to back -- identical shape every time.

## Known limitations (accepted for this milestone only)
No blinking text-input caret (see Rejected alternatives). The cursor
sprite is a plain 8x8 filled square, not a real pointer/arrow shape --
sufficient to PROVE the pipeline (real input -> real pixel movement),
cosmetic polish is future work if wanted. `fb_scroll_up()`'s raw
byte-by-byte row copy is not optimized (no batched word-sized copy) --
fine for a hobby kernel's boot-log volume (CLAUDE.md's stated priority:
correctness/clarity over perf). No MTRR/PAT tuning of the framebuffer's
memory-type attributes (write-back cacheable, same as every other
direct-mapped page) -- adjacent to the ACPI/perf non-goal boundary, not
attempted. `fb_pack_color()`/the framebuffer driver only support 32bpp
direct-color RGB -- an INDEXED-palette or `EGA_TEXT` negotiated mode
(neither ever observed from QEMU's default machine) panics rather than
falling back. The graphics console's visible boot log starts partway
through boot (after the direct-map is ready), not at `kernel_main`'s
very first line, unlike the old VGA console -- every message still
reaches serial throughout, so nothing is silently lost, just not
visually rendered a few lines earlier (see Decision's `fb_ready`
discussion).
