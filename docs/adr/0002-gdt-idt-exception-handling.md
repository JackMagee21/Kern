# ADR 0002: C-managed GDT, IDT, and exception-handler trap frame

## Status
Accepted and verified (Milestone 2) — see Verification for what was
checked pre-toolchain vs. confirmed in a real QEMU boot.

## Context
Milestone 1 got the CPU into 64-bit long mode using a hand-assembled,
throwaway GDT in `boot.asm` just to bootstrap far enough to run C. Roadmap
Milestone 2 ("GDT + IDT + exception handlers") is the one everything
after it depends on: without an IDT, any fault (bad pointer, divide by
zero, bad instruction) triple-faults with zero diagnostic information,
which makes every later milestone (paging, scheduler, syscalls) far
harder to debug.

## Decision

- **C-managed GDT (`gdt.c`), same 3-entry flat layout as boot.asm's
  `gdt64`.** Reason to redo it in C at all: it's explicitly named in the
  Milestone 2 title, and a struct-based table is the same
  explicit-bit-packed-layout discipline the IDT needs, applied
  consistently. Deliberately **no TSS, no ring-3 descriptors** — neither
  is load-bearing until IST stacks or userspace exist, and adding them
  now would be scope creep the milestone doesn't need.
- **IDT: 256 entries, only vectors 0-31 (CPU exceptions) populated.**
  IRQs (vectors 32+) are Milestone 5's job (PIT/APIC). Unpopulated
  entries are left zeroed (not-present) — an unexpected interrupt on an
  empty vector faults into our own #GP/#NP handler and gets a dump
  instead of undefined behavior, which is a safe default requiring no
  extra code.
- **One NASM stub per exception vector (`isr.asm`), not a shared
  vector-number-in-a-register trick**, so a wrong vector number is
  impossible to introduce by miscomputing an offset at runtime — each
  `isrN` is a fixed, individually-named entry point, and `idt.c` maps
  vector→handler via an explicit array of 32 `extern` declarations
  rather than any address arithmetic over a symbol table.
- **Every exception handler is a terminal fault dump.** `isr_handler`
  (exceptions.c) prints the vector, name, error code, `CR2` (page faults
  only), and every register to serial, then halts. No recovery path
  exists yet — there's no scheduler to resume into and no process
  isolation, so "recover and keep going" isn't a real option regardless
  of what the fault was. Per CLAUDE.md safety rule 6, this is the
  intended never-fail-silently behavior for now, not a shortcut to
  revisit immediately.
- **Trap frame layout is an internal contract, not a spec.** Unlike the
  GDT/IDT gate layouts (hardware-mandated, Intel SDM Vol. 3A Sec. 3.4.5 /
  6.14.1), the order `isr_common_stub` pushes GPRs in and the order
  `trap_frame_t` declares them in is entirely our own choice — the only
  correctness requirement is that the two stay in exact agreement, which
  is documented directly in `trap_frame.h` next to the struct.
- **Stack-alignment gotcha handled explicitly, not by luck.** Long mode
  always pushes SS:RSP as part of the exception frame and guarantees RSP
  is 16-byte aligned at the first instruction of the handler — but the
  no-error-code path (which pushes a dummy error code to normalize the
  frame shape) and the has-error-code path arrive at `isr_common_stub`
  8 bytes apart in alignment, and no fixed GPR-push count can make both
  paths land 16-aligned at the `call` (worked out algebraically: one
  path needs an even push count, the other an odd one). So
  `isr_common_stub` saves the true stack pointer in `RBX` (callee-saved,
  so the C handler is contractually required to preserve it), forces
  16-byte alignment with `and rsp, ~0xf` only for the `call`, and
  restores `RSP` from `RBX` afterward before popping registers back.
  This mechanism does not depend on `-mno-sse` making misalignment
  harmless (it's real either way).

## Rejected alternatives
- **Keep using boot.asm's ad hoc GDT permanently** — works, but leaves
  the "real" GDT undocumented/hand-derived forever and contradicts what
  ADR 0001 already committed to. Rejected in favor of formalizing it now
  while it's still simple (3 flat entries), before a TSS/ring-3 rewrite
  would make retrofitting explicit bit-packing more error-prone.
- **A single shared ISR entry point that pushes the vector number via a
  runtime-computed offset** (e.g., one stub reused 32 times via `call`
  displacement tricks) — saves a little code size but makes "which
  vector is this" something you compute instead of something you can
  read off the label name, for no real benefit at 32 entries.
- **Recovering from some exceptions** (e.g., stepping past a benign
  fault) — not attempted; there's no defined "safe state to resume into"
  without a scheduler/process model, so a fake recovery would just
  convert a visible crash into a later, harder-to-diagnose one.

## Verification
- `nasm -f elf64` assembles `isr.asm` and `gdt_flush.asm` cleanly; every
  `isrN` stub's push sequence (dummy-error-code-or-not, correct vector
  number) was checked by disassembling the object directly (`objdump
  -d`), spot-checking `isr8`/`isr14` (error-code vectors) and `isr31`
  (no-error-code) against the `ISR_ERR`/`ISR_NOERR` vector lists.
- The `and rsp,~0xf` / `mov rbx,rsp` / restore-from-`rbx` sequence in
  `isr_common_stub` was verified by disassembly, not just by reading the
  source.
- `boot/linker.ld` + the assembled `boot.o`/`gdt_flush.o`/`isr.o` were
  linked with host `ld` (`--unresolved-symbols=ignore-all`, since the C
  objects aren't buildable yet) purely to exercise the linker script
  structurally: confirms `.text`'s VMA (`0xffffffff8010d000`) minus its
  LMA (`0x10d000`) is exactly `KERNEL_VMA_OFFSET`, and `grub-file
  --is-x86-multiboot2` still accepts the result.
- `libk/fmt.c`'s `u64_to_hex` (used by the fault dump) is genuinely host
  compiled and tested (`tests/host/test_fmt.c`, ASan/UBSan) — it's plain
  C with no hardware dependency, unlike everything else in this ADR.
- **Done, once the `x86_64-elf-gcc`/`binutils` AUR build finished:**
  `make run` boots the real ISO under QEMU (TCG, `-display none -serial
  stdio`) and prints `[OK] hello kernel`, `[OK] gdt/idt installed`, then
  the `int3` self-test's fault dump — `#BP Breakpoint` at vector `0x3`,
  `cs=0x8`/`ss=0x10` matching `KERNEL_CODE_SELECTOR`/`KERNEL_DATA_SELECTOR`
  exactly, sane `rip`/`rsp`/`rbp`, all GPRs present and unmangled.
  `tests/qemu/test_boot_serial.sh` and `tests/qemu/test_idt_selftest.sh`
  both pass (the latter needed one fix: its expected marker string
  wasn't zero-padded to match `serial_write_hex`'s actual 16-digit
  output — a test bug, not a kernel bug).
  This also resolves the one previously-open uncertainty: the "SS:RSP
  always pushed in long mode, handler entry 16-byte aligned" claim
  `isr.asm`/`trap_frame.h` rely on is now confirmed by observation, not
  just documentation — `ss` reading back as exactly `0x10` (rather than
  whatever `push_registers` happened to leave on the stack) is only
  possible if the CPU genuinely pushed it there.
