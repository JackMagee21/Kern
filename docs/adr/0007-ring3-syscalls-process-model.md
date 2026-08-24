# ADR 0007: Ring 3, SYSCALL/SYSRET, and a minimal process model

## Status
Accepted and verified (Milestone 7) — `make run` boots the real ISO, a
ring-3 task runs, its validated syscall prints its message, and its
`sys_nop` loop round-trips millions of times; see Verification. One
real bug was found and fixed via an actual boot, not just review — see
Decision's page-table note.

## Context
Every task through Milestone 6 ran at ring 0 (kernel privilege). This
milestone is the first code in the kernel that runs anything at ring 3,
which needs: a way to enter/leave ring 3 in a controlled way, a way for
ring-3 code to ask the kernel to do privileged things on its behalf
(syscalls), and a story for where ring-3 *code* comes from, since there
is no filesystem or ELF loader yet (CLAUDE.md non-goals: no real FS, no
POSIX userland).

## Decision

- **SYSCALL/SYSRET, not `int 0x80`.** Confirmed with the user directly
  (the harder, more "real" option over reusing the existing IDT
  infrastructure). This meant genuinely new machinery rather than
  extending Milestone 2/5's ISR/IRQ stubs: SYSCALL doesn't go through
  the IDT, doesn't push a frame, and doesn't switch stacks itself.
- **One embedded ring-3 demo task, shared address space** — confirmed
  as the recommended scope when asked (user said "not sure," so this is
  my call, made explicit here rather than silently assumed). No
  fork/exec, no separate per-process page tables/CR3, no filesystem-
  loaded programs. Genuine ring0/ring3 *privilege* separation (ring-3
  code cannot execute privileged instructions or read/write
  supervisor-only memory), but honestly scoped as a single demonstration,
  not a process model in the fuller sense. Revisit if/when something
  actually needs isolated address spaces.
- **GDT extended with a specific, non-obvious ordering SYSRET requires**:
  null, kernel code (0x08), kernel data (0x10), an unused 32-bit-compat
  placeholder (0x18), user data (0x20), user code (0x28), then a 16-byte
  TSS descriptor (0x30). SYSRET reconstructs its target CS/SS from
  `IA32_STAR[63:48]` as `+8`=SS, `+16`=CS(64-bit) — verified against
  Linux's actual `syscall_init()` (`arch/x86/kernel/cpu/common.c`) and
  its GDT layout, not derived from memory alone, since this exact
  ordering is a well-known, easy-to-get-wrong gotcha (get the offsets
  wrong and SYSRET loads the wrong descriptors with no compile-time
  warning). `USER32_CS_PLACEHOLDER` is never loaded by anything in this
  kernel (no 32-bit compat-mode user code exists) — it's a spacer that
  exists purely so the `+8`/`+16` arithmetic lands correctly.
- **64-bit TSS, byte-exact layout verified against Linux's
  `struct x86_hw_tss`** (`arch/x86/include/asm/processor.h`), and the
  16-byte TSS descriptor format verified against Linux's
  `struct ldttss_desc` (`arch/x86/include/asm/desc_defs.h`) — translated
  from Linux's bitfields to explicit byte fields for the same reason
  `gdt_entry_t`/`idt_gate_t` already avoid C bitfields (implementation-
  defined packing order; CLAUDE.md requires hardware layouts not depend
  on that). The TSS is needed regardless of the SYSCALL-vs-`int 0x80`
  choice: any hardware interrupt/exception reaching a ring-3 task
  (Milestone 5's timer, most obviously) still goes through the IDT's
  interrupt-gate mechanism, which unconditionally consults `TSS.RSP0`
  on a ring-3-to-ring-0 transition — SYSCALL itself never touches RSP0.
- **Every ring-3 task gets its OWN dedicated kernel stack, not a shared
  one — this is a real correctness requirement, not a nicety.** Worked
  through by hand before writing code: `TSS.RSP0` is a single fixed
  address the CPU always starts from on every ring3→ring0 interrupt
  transition. If two different ring-3 tasks' preempted contexts both
  used the same RSP0 stack, the second task to be interrupted would
  land its trap frame in the exact same memory where the first task's
  still-pending saved context is sitting, corrupting it. `task_t`
  carries a `kernel_stack_top` field; `scheduler.c`'s timer handler
  updates `TSS.RSP0` (and the analogous syscall-entry kernel stack
  pointer) on every switch, to whichever task is now current. For a
  ring-0 task this field is simply never consulted (same-privilege
  interrupts don't switch stacks), so it's a harmless default there.
- **The ring-3 demo program is hand-written NASM, not a compiled C
  function.** A normal C function (compiled as part of the kernel under
  `-mcmodel=kernel`) lives inside the kernel's own `.text`, which
  boot.asm maps with 2MiB huge pages, supervisor-only. Marking "just
  this one function's page" user-accessible isn't possible without
  exposing the *entire* 2MiB page's worth of other kernel code/data
  sharing it — a serious privilege leak, not a minor wart. Instead,
  `kernel/sched/user_demo.asm` is linked as a normal kernel object
  (`boot/linker.ld`'s `.user_demo` section, padded to its own page(s) so
  nothing else shares that physical frame), then `task_create_user()`
  maps that SAME physical page a second time at a new user-region
  virtual address with the U bit set — the original kernel-higher-half
  alias stays supervisor-only and simply goes unused. The blob uses
  `default rel` for all internal references (needs to be genuinely
  position-independent, since it executes from a different virtual
  address than where the linker placed it) and never references
  anything outside itself, since `syscall` needs no address.
- **`vmm_map_page()`'s U-bit propagation upgrades pre-existing
  intermediate entries, not just newly-created ones — found via an
  actual boot, not caught in review.** First attempt only set the U bit
  when *creating* a fresh intermediate table entry, reasoning (on paper)
  that a "dedicated" user PDPT slot would never share a table with
  existing mappings. That reasoning had a real gap: `PML4[511]` alone
  spans the *entire* top-2GiB kernel region under `mcmodel=kernel` —
  the kernel image, the heap, AND every "dedicated" user PDPT slot all
  fall under the one same PML4 entry, which the heap's own earlier
  mapping calls had already created supervisor-only. First boot faulted
  immediately (`#PF` at the ring-3 entry point itself, error code
  `0x5` = present + protection violation + user-mode access) — exactly
  what a missing U bit on a shared intermediate level looks like.
  Fixed by upgrading (OR-ing in) the U bit on an already-present
  intermediate entry when a user mapping needs to pass through it —
  safe, because the U bit is necessary-but-not-sufficient at every
  level: every *other* region's own leaf PTEs stay supervisor-only and
  independently block ring-3 access regardless of what an intermediate
  table permits.
- **`vmm_is_user_range()` validates every page in a range (present AND
  user-accessible at every level, correctly handling boot.asm's 2MiB
  `PS` pages as their own leaf), called before `sys_write` ever
  dereferences a user-supplied pointer.** Directly implements CLAUDE.md's
  security rule ("never dereference user-supplied pointers/lengths
  without validating they're user-accessible") — checking "is this a
  valid kernel address" would not be enough, since a buggy or malicious
  user program could pass a *kernel* address and have the syscall
  read/write it with the kernel's own privilege.
- **Standard Linux x86_64 syscall register convention adopted
  deliberately** (`RDI/RSI/RDX/R10/R8/R9` for args, `R10` not `RCX` for
  arg 4 specifically because `RCX` is clobbered by `SYSCALL` itself,
  `RAX` for number-in/return-out) — reusing a well-established, already-
  correct-by-design convention is simpler than inventing a new minimal
  one, not scope creep.
- **`IA32_FMASK` clears `IF` (and `TF`, defensively) on `SYSCALL`
  entry.** `SYSCALL` leaves `RSP` unchanged — still the untrusted user
  stack — until `syscall_entry.asm` manually switches to a kernel stack.
  If an interrupt fired in that window, the CPU (already at CPL 0, so a
  same-privilege interrupt) would push its frame using whatever `RSP`
  currently holds, i.e. the user's stack — a well-documented
  SYSCALL/SYSRET gotcha. Masking `IF` closes that window entirely.
- **Syscalls are non-preemptible and never switch tasks in this
  milestone.** `syscall_entry.asm` always resumes the exact context it
  saved (restores `RSP` from `RBX`, the pre-align value — same pattern
  `common_stub.inc` used before Milestone 6 simplified it to a return-
  value-driven resume), unlike `isr_handler`/`irq_handler` which can
  return a different frame. Simpler and lower-risk for a first cut;
  every syscall body here is a handful of instructions, so an unbounded
  latency concern doesn't yet exist. Revisit if/when a syscall needs to
  block or run long enough that this matters.

## Rejected alternatives
- **`int 0x80`** — rejected by explicit user choice; would have reused
  Milestone 2/5's existing trap-frame infrastructure directly (just a
  new DPL=3 gate), significantly less new machinery.
- **A full process model** (per-process address spaces, fork/exec) —
  no filesystem exists to load programs from regardless (non-goal), and
  nothing yet needs isolated address spaces given there's exactly one
  demonstration ring-3 task. Revisit alongside a real FS/ELF loader.
- **NX-bit enforcement on the user code page** — the U bit alone
  governs ring-3 read/execute access; nothing in this milestone marks
  the *data* pages (user stack) non-executable via the NX bit (would
  need `EFER.NXE` and bit 63 of the leaf PTE). Accepted gap for now —
  the demo program doesn't need it, and the general "validate before
  trusting" discipline (`vmm_is_user_range`) is the load-bearing
  security property here, not W^X. Worth adding once something
  executes attacker-influenced data (e.g., real user programs).
- **Chained/multiple IRQ handlers per line** — unrelated to ring 3
  itself; not revisited here, still out of scope per ADR 0005/0006.

## Verification
- STAR/LSTAR/SFMASK MSR numbers, the STAR encoding, `EFER.SCE`'s bit
  position, the TSS struct layout, and the TSS descriptor format were
  all verified against Linux kernel source
  (`msr-index.h`, `cpu/common.c`, `processor.h`, `desc_defs.h`), not
  recalled from memory — the same primary-source discipline as every
  prior ADR's hardware facts.
- `kernel/sched/user_demo.asm` verified by disassembly: `lea
  rdi,[rip+...]` confirms RIP-relative (position-independent)
  addressing for its internal `msg` reference.
- `readelf`/`nm` on the linked `kernel.elf` confirmed `.user_demo` is
  exactly one 4KiB page (`user_demo_end_lma - user_demo_start_lma =
  0x1000`), with `user_demo_start` at offset 0 — nothing else shares
  that physical frame.
- `syscall_entry.asm` verified by disassembly: the save/align/call/
  restore/`sysretq` sequence encodes as expected (`48 0f 07` for
  `sysretq`).
- `make run` (real toolchain) boots and prints, after all Milestones
  1-6 markers unchanged: `[OK] tss/syscall initialized`, `[OK] scheduler
  initialized, 2 kernel + 1 ring-3 demo task created`, `[OK] hello from
  ring 3 via syscall` (the validated `sys_write`), then (after the
  existing timer/scheduler self-tests) `[OK] syscall self-test passed,
  0x223628 syscalls serviced from ring 3` — millions of `sys_nop`
  round-trips, proving `SYSCALL`/`SYSRET` is reliable under sustained
  use, not just correct once.
- The first real boot attempt faulted (`#PF`, vector `0xe`, error code
  `0x5`, at the ring-3 entry point itself) — diagnosed via the exact
  Milestone 2 fault-dump infrastructure this whole roadmap has been
  building toward, confirming the PML4-sharing gap described above.
  Fixed, then re-verified with a clean boot.
- `tests/qemu/test_ring3_syscall_selftest.sh` (new): checks all the
  markers, that the ring-3 message appears *after* task creation (real
  sequencing, not a coincidental substring match), and that the syscall
  count is well beyond 1 (proving the loop, not just the one-shot call).
- All six earlier milestones' smoke tests re-run and pass (one, the
  Milestone 6 test, needed its own expected-marker string updated after
  `kernel_main`'s log line changed to mention the new ring-3 task — a
  test-text fix, not a behavior regression). Both host tests
  (`test_fmt`, `test_heap_alloc`) re-run and still pass.
