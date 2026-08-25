# ADR 0027: minimal display server, one client, no overlap

## Status
Accepted and verified — `make run` boots the real ISO; a genuinely
separate display-server process (`kernel/user/display_server.c`) claims
sole ownership of the real graphics framebuffer, and a genuinely
separate client process (`kernel/user/display_client.c`) asks for a
canvas larger than the server is willing to grant, receives a smaller
one back, and gets exactly that smaller rectangle composited onto the
real screen — verified pixel-for-pixel via a real QEMU `screendump`
(the same technique Milestone 23 established), not just by the demo's
own self-reported success markers. Ownership exclusivity is proven both
within a single process (the server's own second `sys_fb_acquire()`
call fails) and across processes (the client's own attempt fails too,
causally after the server's successful one — no scheduling-order
assumption needed). All twenty-six smoke tests (twenty-five
pre-existing plus the new `test_display_server_selftest.sh`) and all
four host test suites pass. `-d int,cpu_reset` trace unchanged from
Milestone 26 (1 `#BP`, 3 `#PF`, zero kernel-caused resets). Booted
clean on the first real attempt, then 3 additional repeat boots with
identical shape every time — no bugs found this milestone that weren't
caught in review before ever running.

## Context
Fourth step of the GUI arc (`Desktop.md`), and its own explicitly
flagged "actual hard-unknown milestone": prove the client-server
display model works at all before any multi-window logic
(z-order, damage tracking, input focus — `Desktop.md`'s milestone 5)
gets built on top of it. Builds on every prior GUI-arc milestone at
once: the userspace C runtime (24) to write this in C rather than raw
NASM, the blocking/wake primitive (25) underneath `sys_ipc_recv`'s
blocking, and IPC + shared memory (26) as the actual transport — a
client's pixel buffer is a shared-memory object, and the
request/grant/present handshake is three ordinary IPC messages.
Deliberately narrow scope: one server, one client, no window list, no
damage tracking, no chrome, fixed canvas position — isolating this
milestone's own real risk (does the client-server split work at all?)
from every later one.

## Decision

- **The kernel is the sole framebuffer owner and the sole pixel
  writer; two new syscalls, not a new subsystem.** Considered mapping
  the raw physical framebuffer MMIO directly into the display server's
  own address space (letting it call `fb_put_pixel`-equivalent logic
  itself, ring-3). Rejected: it would require duplicating
  `kernel/drivers/framebuffer.c`'s own channel-packing logic
  (`fb_pack_color`'s negotiated bit-position/size fields, Milestone 23)
  in userspace, breaks this codebase's established "kernel owns
  hardware access" architecture for zero benefit this milestone
  actually needs, and turns a straightforward validated-buffer-copy
  syscall into a much larger surface (raw MMIO reachable from ring 3).
  Instead: `SYS_FB_ACQUIRE` (no args; succeeds exactly once, ever, for
  the whole boot — the first caller of ANY process becomes the sole
  owner, checked by pid on every subsequent call, not just "is anyone
  the owner" — returns `(fb_get_width() << 32) | fb_get_height()`,
  packed the same way `MSR_STAR`'s two selector halves already are, or
  `(uint64_t)-1` on failure) and `SYS_FB_PRESENT` (`x, y, w, h, buf_va`
  — validates the caller is the current owner and `[buf_va, buf_va +
  w*h*4)` is a fully validated user range in the CALLER's own address
  space via the EXISTING `vmm_is_user_range()`, CLAUDE.md's "never
  dereference a user-supplied pointer/length without validating it"
  rule — then loops `fb_put_pixel(x+px, y+py, fb_pack_color(r,g,b))`
  per pixel, reading each `0x00RRGGBB` entry from the caller's own
  buffer). Individual out-of-screen pixels are silently dropped by
  `fb_put_pixel()`'s OWN pre-existing bounds check — no separate
  screen-edge clamp needed in the new syscall at all.
- **Ownership exclusivity is a KERNEL-enforced invariant, not a
  userspace convention.** `fb_owner_pid`, a single kernel-global set
  once by whichever process's `sys_fb_acquire()` call runs first, never
  reset (not even on that process's own exit — see Known limitations).
  "One server process owns the framebuffer" (`Desktop.md`'s own
  phrasing) is real: a second process, buggy or not, genuinely cannot
  ever call `sys_fb_present()` successfully, checked at the syscall
  boundary regardless of what any userspace code does or doesn't do.
- **The actual size-limiting POLICY — "the server enforces the
  bound" — lives in userspace (`display_server.c`'s own fixed
  `MAX_CANVAS_W`/`MAX_CANVAS_H`), not in the kernel syscall.** The
  kernel's `sys_fb_present` faithfully blits whatever a validated owner
  asks it to (ownership + memory-safety are the ONLY things it
  enforces); the demo's own display server is what refuses to ever
  grant a client more than 200x150 regardless of what's requested,
  exactly matching `Desktop.md`'s framing of this as the milestone
  proving the CLIENT-SERVER MODEL, not a kernel safety feature. A real
  window manager's own placement/sizing policy will live in userspace
  the same way once `Desktop.md`'s milestone 5 (multi-window) exists.
- **A tiny three-message protocol (`kernel/user/display_protocol.h`),
  layered entirely on the EXISTING `ipc_message_t`/`sys_ipc_send`/
  `sys_ipc_recv` mechanism (Milestone 26) — no new IPC machinery.**
  `DISPLAY_OP_REQUEST` (client → server: wanted w/h), `DISPLAY_OP_GRANT`
  (server → client: granted x/y and, packed into `fields[3]`, granted
  w/h), `DISPLAY_OP_PRESENT` (client → server: an shm id, already sized
  to and filled with EXACTLY the granted canvas). The client never
  allocates a buffer larger than what it was actually granted — the
  bound-enforcement proof this milestone's smoke test performs (a real
  screendump bounding-box check) is airtight for a structural reason,
  not just a runtime clamp: there is no memory anywhere in this demo
  containing pixels beyond the granted rectangle for a bug to
  accidentally leak onto the screen.
- **Ordering is enforced by the protocol's own blocking IPC, not by
  process-creation order.** Milestone 26's ADR found a real bug from
  assuming (wrongly) which of two processes would run first;
  deliberately avoided repeating that class of mistake here by
  reasoning through why it doesn't matter this time: the client's own
  `sys_ipc_recv()` for the server's `DISPLAY_OP_GRANT` genuinely blocks
  until the server has ALREADY completed its own `sys_fb_acquire()` (a
  precondition of the server even reaching its own receive-then-reply
  code), so the client's own later cross-process ownership-rejection
  check is causally guaranteed to run after the server's successful
  acquire — correct by construction, regardless of which of the two
  processes the scheduler actually happens to run first. The only
  ordering constraint that DOES matter is a compile-time one: the
  server must be `task_create_user_image()`'d before the client, purely
  so `kernel_main` knows its pid to inject into the client's bootstrap
  message.

## Rejected alternatives
- **Mapping the raw physical framebuffer into the server's own address
  space.** See Decision above — bigger MMIO-in-ring-3 surface, forces
  duplicating channel-packing logic in userspace, no benefit this
  milestone needs.
- **A kernel-side maximum-canvas clamp inside `sys_fb_present` itself**
  (in addition to the userspace policy). Rejected as redundant for this
  milestone: the demo's own protocol already makes it structurally
  impossible for the client to ever hand the server a buffer bigger
  than what was granted, and `fb_put_pixel`'s own existing bounds check
  already protects the real screen edges for free. A kernel-enforced
  per-owner canvas cap is reasonable future work if/when a LESS
   trusted client than this milestone's own demo needs to be defended
  against, not needed now (CLAUDE.md: don't build for a hypothetical
  future requirement).
- **Releasing framebuffer ownership when the owning process exits.**
  Rejected as unnecessary machinery for this milestone: the display
  server is expected to run for the rest of the kernel's uptime, the
  same assumption `kernel_main`'s own shell already makes about
  itself. A real "restart a crashed display server" story is future
  work — see Known limitations.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-26
  marker unchanged, plus: `[OK] display server/client processes
  created...`, `[OK] display server: framebuffer acquired`, `[OK]
  display server: a second sys_fb_acquire correctly failed...`, `[OK]
  display client: sys_fb_acquire correctly rejected...`, `[OK] display
  client: canvas presented via the display server`, `[OK] display
  server: presented the client's granted canvas`, and `[OK] display
  server self-test passed, sys_fb_present blitted 0x1 frame(s)...`, all
  in the correct sequence, before the process-lifecycle self-test's
  reap-count check (raised 7 → 9) passes.
- **`tests/qemu/test_display_server_selftest.sh` (new)**: the real,
  pixel-level proof. Boots headless, waits for the shell prompt (so
  every boot-time self-test has already settled), takes a real QEMU
  monitor `screendump`, and scans the ENTIRE captured framebuffer for
  the client's distinctive fill color (a teal chosen to be far from
  both the black background and the cursor's solid red,
  Milestone 23) — then asserts the resulting bounding box is EXACTLY
  `(100, 100)` to `(299, 249)`, 30000 pixels — the server's granted
  200x150 canvas, NOT the 400x300 the client originally asked for. Also
  re-asserts every earlier marker, the reap count (now 9), and the
  process-lifecycle frame-leak self-test.
- `-d int,cpu_reset` trace: unchanged from Milestone 26 (1 `#BP`, 3
  `#PF`, zero kernel-caused resets — the two `CPU Reset` lines QEMU's
  own `-d cpu_reset` logs at every boot, BEFORE the kernel ever runs
  [`CR3=0`, real-mode BIOS reset vector `EIP=0xfff0`], are QEMU's own
  standard pre-boot machine-init noise, present on every x86 QEMU boot
  regardless of guest kernel — not a new phenomenon this milestone
  introduced, confirmed by inspecting the actual register state in
  both entries rather than assumed).
- All twenty-five earlier smoke tests and all four host test suites
  re-verified passing on a clean rebuild. Booted clean on the FIRST
  real attempt (no bugs found live this milestone — every design
  question, e.g. the causal-ordering reasoning above, was worked
  through in review before ever running QEMU), then 3 additional
  repeat boots, identical marker sequence and `0x1` present-count every
  time.

## Known limitations (accepted for this milestone only)
Exactly one server, one client — no window list, no z-order, no damage
tracking, no input routing (`Desktop.md`'s own next milestone). Canvas
position is a fixed constant (`CANVAS_X`/`CANVAS_Y` in
`display_server.c`), not negotiated or movable. Framebuffer ownership
is never released, even after the owning process exits — a crashed or
exited display server permanently locks out any future one for the
rest of this boot; acceptable since nothing in this milestone restarts
a display server, not acceptable once one might. `sys_fb_present`'s own
buffer-validity check trusts `w`/`h` up to a generous sanity cap
(`FB_PRESENT_MAX_DIM = 2048`) before validating the real backing
memory — fine for this milestone's single, cooperating demo client, not
a substitute for a real per-owner canvas cap if a genuinely untrusted
client existed. No damage/dirty-rectangle tracking — every present
re-blits the entire granted rectangle, fine at this milestone's single-
client, single-present scale, not remotely efficient for a repainting
UI later.
