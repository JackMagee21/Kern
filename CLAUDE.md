# CLAUDE.md — x86_64 Bare-Metal C Kernel

## Project
Hobby OS kernel, freestanding C11, x86_64. Priority: correctness/clarity of boot→memory→sched path > extensibility > perf/features.
Non-goals (flag before doing): SMP, ACPI power mgmt, POSIX userland, real FS, USB/net. Don't pull forward without confirmation.

## Environment
Host: Arch Linux on WSL2. Package manager: `pacman` (+ AUR helper, e.g. `yay`, if present).
- No official Arch repo package for `x86_64-elf-gcc` — install from AUR (`yay -S x86_64-elf-gcc x86_64-elf-binutils x86_64-elf-gdb`) or build via OSDev.org `crosstool-ng`/GCC-from-source script. Verify AUR package exists before assuming; if unavailable, build from source.
- `pacman -S nasm qemu-full xorriso mtools nasm gdb make` (qemu-full needed for `qemu-system-x86_64`; xorriso+mtools needed for GRUB/Limine ISO creation).
- WSL2 has no KVM by default — do NOT pass `-enable-kvm` unless nested virtualization is confirmed working; assume TCG (software) emulation, which is slower but fine for dev.
- No GUI passthrough assumed — prefer `-display none -serial stdio` or `-nographic` over a GTK/SDL QEMU window unless WSLg is confirmed working.
- Keep the repo on the Linux filesystem (`/home/...`), not `/mnt/c/...` — cross-compiling and make on the Windows-mounted 9p filesystem is markedly slower and can trip up file-watchers.
- Line endings: enforce LF (`.gitattributes`) — Windows-side editors can introduce CRLF into asm/linker scripts and break tools that are picky about it.

## Toolchain
- Compiler: `x86_64-elf-gcc` cross toolchain ONLY. Verify with `x86_64-elf-gcc -dumpmachine` != host triple. Never use host gcc/clang.
- Flags: `-ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -msoft-float -Wall -Wextra -Werror`
- Asm: NASM. Bootloader: Limine (preferred) or GRUB2/Multiboot2 — don't mix. Emulator: QEMU (TCG on WSL2, see Environment). Debugger: GDB via QEMU stub.

## Layout
```
/boot/            bootloader cfg, linker.ld
/kernel/arch/x86_64/  GDT, IDT, paging, context switch, boot.S
/kernel/drivers/  console, PIC/APIC, PIT/HPET, keyboard
/kernel/mm/       frame allocator, vmm, heap
/kernel/sched/    task struct, scheduler
/kernel/kernel.c  kernel_main, init sequencing
/libk/            freestanding mini-libc
/tests/host/      host-compiled unit tests (allocators, data structs)
/tests/qemu/      boot smoke tests, serial-output assertions
/docs/adr/        design decision notes
```
Factor host-testable logic (allocators, parsers, data structures) out of kernel-only code into `/libk` or plain C so `/tests/host` can compile+test it with host gcc + ASan/UBSan.

## Code rules
- No hosted headers (`stdio.h`, `stdlib.h`, `string.h`) — use `/libk`. Freestanding `stdint.h/stddef.h/stdbool.h` OK.
- No malloc/free until kernel heap milestone lands; no FP/SSE until FPU context save/restore exists.
- snake_case fns/vars, SCREAMING_SNAKE macros/consts, `_t` suffix on typedefs. Small single-purpose functions.
- MMIO/hardware regs: `volatile` pointers or explicit port in/out helpers only.
- Non-trivial subsystem (paging, sched, allocator) → short ADR in `/docs/adr/` with approach + rejected alternatives.

## Kernel safety rules (non-negotiable)
1. Data shared with an interrupt handler: disable interrupts around critical section or use a justified lock-free structure.
2. Know stack size for every context (boot, per-IST exception stack, per-thread later). No unbounded recursion in boot/interrupt paths.
3. Hardware-mandated alignment (GDT/IDT descriptors, 4KiB page tables) enforced explicitly via `_Alignas`/linker script — don't rely on compiler defaults.
4. Paging changes: high blast radius, often manifest as silent triple fault. Test incrementally in QEMU with `-d int,cpu_reset` before trusting.
5. Keep interrupts-disabled sections as short as provable; comment why.
6. On unrecoverable error: print full state (regs, fault addr, backtrace) to serial before halt. Never fail silently, never auto-reboot during dev.

## Build/run/debug
```sh
make            # build/kernel.elf, build/os.iso
make run        # qemu-system-x86_64 -cdrom build/os.iso -serial stdio -no-reboot -no-shutdown -display none
make debug      # same + -s -S; then: x86_64-elf-gdb build/kernel.elf -ex "target remote :1234"
```
(No `-enable-kvm` — see Environment. `-display none` avoids depending on WSLg.)
Debug-only QEMU flags (not default): `-d int,cpu_reset` (log interrupts/resets), `-monitor stdio`.

## Testing
- Host tests (`/tests/host`): allocator logic, data structures, string/printf — run with ASan/UBSan.
- QEMU smoke tests (`/tests/qemu`): boot headless, assert on serial markers (e.g. `[OK] paging init`).
- Every milestone adds/extends a QEMU smoke test proving the milestone's behavior, not just compilation.

## Security
- Validate all sizes/offsets in any parser (ELF loader, future FS) before use.
- Once userspace exists: never dereference user-supplied pointers/lengths without validating they're user-accessible.
- Design ring 0/ring 3 separation into paging now, even pre-userspace, to avoid a later rewrite.

## Roadmap (sequenced hardest-unknown / highest-leverage first)
1. Boot → "hello kernel" via serial (proves toolchain/boot chain)
2. GDT + IDT + exception handlers (visibility into faults — everything else depends on this)
3. Physical frame allocator
4. Paging/VMM + kernel heap
5. PIT/APIC timer + IRQ handling
6. Preemptive scheduler + context switch (single CPU)
7. Userspace: ring 3, syscalls, process model
8. Later: FS, drivers, SMP (sequence TBD from what's learned above)
Full breakdown (deliverables/acceptance/estimates/risks) → `/docs/roadmap.md`, not here.

## Process discipline (bug prevention)
- One subsystem per change. Don't touch paging + scheduler + a driver in one diff — if it breaks, you won't know which part.
- After every change to asm/linker script/struct layout: rebuild clean (`make clean && make`) before testing — stale objects mask real breakage and fake fixes.
- "Compiles" is not "correct," especially for asm, linker scripts, and packed structs — no compile error is emitted for wrong bit layout, wrong struct packing, or wrong calling-convention assumptions.
- Never guess hardware/ABI facts (register layout, instruction semantics, struct bit-offsets, calling convention) from memory when precision matters — cross-check against the Intel/AMD SDM or System V x86_64 ABI before writing GDT/IDT/page-table/ISR code. State the source of truth used, or flag as unverified.
- If a fix isn't understood, don't apply it speculatively (flipping compiler flags, reordering pushes, adding random `volatile`) hoping it helps — that hides bugs instead of fixing them. Diagnose first (see Debug Systematically below), then fix.
- Before declaring a milestone done: actually run the QEMU smoke test and show its output — don't infer success from the code looking right.

## Known x86_64 gotchas (check explicitly, don't assume)
- ISR/IRQ entry stubs: stack must be 16-byte aligned *at the `call`* per SysV ABI before jumping into C handlers — a `push` in the stub (error code, int number) shifts alignment; account for it or the C handler's prologue (esp. any SSE spill, even if unused) can silently misbehave.
- Trap frame field order pushed in the asm stub must exactly match the C struct reading it — a mismatched order corrupts register values with no error, symptoms surface far later (register corruption after IRQ return).
- GDT/IDT entries and page table entries: use exact bit-packed layouts (`__attribute__((packed))`, explicit `uint64_t` fields with documented bit ranges) — never let struct padding/alignment be implicit.
- Page table changes: TLB is not automatically coherent — `invlpg`/reload CR3 after remapping, or you'll silently keep using the stale mapping.
- `iretq` requires the exact interrupt frame layout (RIP, CS, RFLAGS, RSP, SS) in the exact order — get one field wrong and you triple-fault or return to garbage.
- Segment selectors after entering long mode: verify CS/DS/SS are reloaded to the intended GDT entries — a stale selector from the bootloader's GDT is a classic silent bug.
- Multiboot2/Limine header must be within the first N KB of the image per spec and correctly checksummed/aligned — verify against the current spec, not memory of an older version.

## WSL/QEMU hiccups to check first
- Stale `qemu-system-x86_64` process left running (from an earlier `make debug`) blocks GDB port 1234 or holds the ISO file — `pkill qemu-system-x86_64` if a run behaves inexplicably.
- Stale ISO: if the Makefile doesn't correctly depend on all inputs, `make run` can boot an old image — `make clean` before trusting a "still broken" result.
- Cross-compiler picked up from `$PATH` may resolve to a distro package with a different libgcc/multilib config than expected — confirm `x86_64-elf-gcc -print-prog-name=cc1` points where expected if behavior seems toolchain-related.

## Git
One logical change per commit. Imperative summary line + why-focused body. Commits touching paging/boot/interrupts: state how it was tested (QEMU flags, expected output).

## Always
- Check `/docs/adr/*` before modifying a subsystem that has one.
- State which milestone a change belongs to; flag if jumping ahead.
- Paging/interrupt/boot changes: state verification method before calling it done.
- Prefer host-testable modules over kernel-only code where logic doesn't need privileged context.
- New dependency / boot protocol / arch target → flag as a decision needing confirmation.

## Never
- Compile kernel code with host compiler.
- Use FP/SSE before FPU context-switch milestone exists.
- Expand scope silently (e.g. SMP/FS "while in there").
- Let a panic/fault path fail silently.
- Treat "boots in QEMU" as proof of correctness for paging/interrupt code without reasoning through invariants by hand.
