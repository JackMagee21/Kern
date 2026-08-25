# ADR 0024: Minimal userspace C runtime

## Status
Accepted and verified — `make run` boots the real ISO; `kernel/user/
hello.asm` (Milestone 17's hand-written NASM demo) was REPLACED,
byte-for-byte, by `kernel/user/hello.c` compiled through the new
runtime, and every existing test that asserts on this program's output
(`test_elf_loader_selftest.sh`, `test_ring3_syscall_selftest.sh`,
`kernel/kernel.c`'s own self-tests) passes with ZERO assertion changes
— the strongest available proof the runtime produces a correct,
equivalent program. All twenty-three pre-existing smoke tests and all
four host test suites re-verified passing. `-d int,cpu_reset` trace
unchanged from Milestone 23 (1 `#BP`, 3 `#PF`, zero double-fault/
reset). Correct on the first real boot attempt; booted 4 times back to
back with identical shape every time.

## Context
`Desktop.md` scopes a multi-milestone arc toward a usable multi-window
desktop. Every user program shipped so far (`kernel/user/*.asm`) is
hand-written NASM — tractable for four small, linear demo programs, not
for what's coming (a window server with a window list, rectangle math,
damage tracking, and message parsing; several real applications).
Writing and debugging that much logic in raw assembly, with no local
variables worth the name and no structs, would be disproportionately
harder than the actual problems those milestones need to solve.

## Decision

- **A small, custom syscall-wrapper runtime, explicitly NOT the "POSIX
  userland" CLAUDE.md non-goal.** `kernel/user/rt/syscall.h/.c` wraps
  this kernel's own six syscalls (`SYS_NOP`/`SYS_WRITE`/`SYS_EXIT`/
  `SYS_FORK`/`SYS_WAIT`/`SYS_EXEC`) with matching C signatures — nothing
  resembling POSIX file descriptors, signals, or process APIs. Flagged
  explicitly in `Desktop.md` before this milestone started, per
  CLAUDE.md's "new dependency/runtime -> flag as a decision needing
  confirmation."
- **`kernel/user/rt/crt0.asm`: a tiny NASM entry stub, not C.** A C
  function's own prologue assumes it was entered via `call` (RSP%16==8
  at entry, accounting for the pushed return address); every user
  process is actually entered via `iretq`/`sysretq` (no return address
  pushed), so raw RSP is 16-aligned instead of 8-aligned at entry.
  `crt0.asm` does the exact same `and rsp, ~0xf` fix
  `kernel/arch/x86_64/boot.asm`'s own `higher_half_entry` already
  applies before ITS first C call — the established pattern for this
  precise class of ABI boundary in this codebase, not a new one.
- **`-mcmodel=large`, not `=kernel` or the default small model, for all
  userspace C code.** Process-private code is architecturally
  constrained to live at PML4 index >= 1 (index 0 is committed to the
  shared identity map, ADR 0009) — i.e. virtual addresses >= 0x8000000000,
  far outside what small/kernel-model 32-bit-displacement addressing can
  reach. This is exactly why every hand-written `.asm` user program
  already needed `default rel` (`hello.asm`'s own doc comment, before
  its retirement this milestone) — `-mcmodel=large` is the C-compiler
  equivalent: full 64-bit immediate addressing, correct regardless of
  link address. Verified concretely, not just reasoned about: `readelf`/
  `nm` on the linked `hello.elf` confirms `_start` at `0x8000400000`
  (`user.ld`'s existing base) with no truncated-relocation link errors,
  and the actual QEMU boot proves the resulting code runs correctly at
  that address.
- **No `-mno-red-zone` for userspace C code, deliberately differing
  from the kernel's own `CFLAGS`.** The red-zone hazard the kernel side
  disables it for (an interrupt landing on the SAME stack a leaf
  function was using its red zone on) doesn't apply to ring-3 code:
  every trap into the kernel switches to the kernel's own `TSS.RSP0`
  stack first (ADR 0007), never touching the user stack's red zone.
  Reasoned through explicitly rather than reflexively copying the
  kernel's flags.
- **A new `kernel/user/rt/` directory, distinct from `/libk`.** `/libk`
  is kernel-side code by its own stated purpose (freestanding,
  host-testable where the logic doesn't need privileged context); this
  runtime is ring-3-only and needs its own compile flags entirely
  (`USER_CFLAGS` in the `Makefile`, a static pattern rule scoped to
  exactly the files that need it — chosen over relying on GNU Make's
  implicit-rule stem-length precedence for the same effect, since an
  explicit target list is far more legible to a future reader than that
  subtlety would be).
- **`kernel/user/rt/string.c`: a minimal `memset`/`memcpy`/`memmove`/
  `strlen` subset**, both for explicit use and defensively — a
  freestanding C compiler can synthesize implicit calls to
  `memcpy`/`memset`/`memmove` for struct assignments or array
  initializers even in code that never calls them explicitly, the same
  reason kernel-side code needs `/libk` rather than trusting none of
  these ever get emitted.
- **`hello.asm` retired (deleted), replaced in place by `hello.c`** —
  not kept alongside as a second, parallel demo. The Makefile's
  existing `$(USER_ELF): ... hello.o ...` link rule needed only its
  input object list extended (the three new runtime objects prepended);
  `kernel/mm/elf_loader.c` doesn't care whether the bytes it loads came
  from NASM or a C compiler, so no kernel-side change was needed at
  all.

## Rejected alternatives
- **Writing the window server and future apps directly in more
  hand-tuned NASM**, deferring a C runtime indefinitely. Rejected — see
  Context; the actual GUI-arc problems (window lists, rectangle
  intersection, message parsing) are the kind of logic assembly makes
  disproportionately harder and more error-prone to get right, not
  faster.
- **A new demo program to prove the runtime, instead of rewriting
  `hello.asm` in place.** Rejected in favor of the in-place rewrite:
  reusing `hello.asm`'s own already-tested exact behavior (two specific
  messages, a `.data`/`.bss` correctness check, a bounded `sys_nop`
  spin every existing self-test already counts on) as the runtime's own
  proof is strictly stronger than a new program with new, unproven
  assertions — zero test files needed any assertion changes at all.
- **Reusing the kernel's own `CFLAGS`** for userspace C code. Rejected
  — wrong code model (`=kernel` assumes the -2GB region, not where user
  code lives) and an unnecessary red-zone restriction that doesn't
  apply to ring-3 code (see Decision).

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-23
  marker unchanged, including BOTH `hello.elf` instances' exact
  messages (`[OK] hello from ring 3 via ELF-loaded process` / `[OK] elf
  .data/.bss segment verification passed`, each exactly twice) — now
  produced by compiled C instead of hand-written NASM, with the
  underlying bytes verified different (a real recompile happened, not
  an accidental no-op) via the build log showing `x86_64-elf-gcc`
  invoked on `hello.c`.
- `readelf -h`/`readelf -S`/`nm` on the linked `hello.elf`: entry point
  and every section at the expected addresses (`_start` at
  `0x8000400000`, matching `user.ld`'s existing base), confirming the
  large-code-model link actually produced the intended layout before
  ever booting it.
- `-d int,cpu_reset` trace across a full boot: unchanged from Milestone
  23 (1 `#BP`, 3 `#PF`, zero double-fault/reset) — this milestone
  introduced no new fault-driven code paths.
- All twenty-three pre-existing `tests/qemu/*.sh` smoke tests and all
  four host test suites re-verified passing with ZERO assertion
  changes — the strongest possible regression signal for a runtime
  swap underneath an already-tested program. Booted 4 times back to
  back — identical shape every time.

## Known limitations (accepted for this milestone only)
Only two syscalls' worth of argument-passing capacity (`syscall2`,
`rdi`/`rsi`) — sufficient for every syscall this kernel has today, but
would need extending (to `r10`/`r8`/`r9`, matching the SysV syscall
convention's full six-argument capacity) the first time a future
syscall needs more than two arguments; not built preemptively (YAGNI).
No `argv`/`envp`/heap allocator/dynamic memory in the runtime yet — not
needed by `hello.c`'s own rewrite; the GUI arc's later milestones will
add whatever a window server/app actually turns out to need, not
before. `sys_exit`'s trailing infinite loop (defensive, matching every
hand-written `.asm` demo's own `.hang:` convention) is unreachable if
`sys_exit` works, which it always has since Milestone 10.
