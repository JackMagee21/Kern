# Roadmap

Sequenced hardest-unknown / highest-leverage first, per CLAUDE.md. Each
milestone's deliverable must be proven by an actual QEMU smoke test in
`/tests/qemu`, not just "it compiled."

## 1. Boot → "hello kernel" via serial — DONE (this change)
**Proves:** toolchain + boot chain (cross-compiler, NASM, GRUB2/Multiboot2,
higher-half long-mode transition) all actually work together.
**Deliverables:**
- `boot/linker.ld`, `boot/grub.cfg`
- `kernel/arch/x86_64/boot.asm`: Multiboot2 header, 32-bit entry, CPUID/
  long-mode checks, boot page tables, GDT64, transition to long mode,
  jump to higher-half `kernel_main`.
- `kernel/drivers/serial.c/.h`: polling 16550 UART driver (COM1).
- `kernel/kernel.c`: `kernel_main`, prints `[OK] hello kernel`, halts.
- `libk/io.h`: `inb`/`outb`/`io_wait`.
**Verification:** `make check-mb2` (grub-file), `tests/qemu/test_boot_serial.sh`
(boots headless, greps serial output for the marker).
**Design record:** `docs/adr/0001-boot-protocol-and-long-mode-entry.md`.
**Known limitation (accepted for this milestone only):** boot page tables
only map the low 8MiB of physical memory (2MiB pages, no 4KiB level) —
replaced by a real VMM in Milestone 4.

## 2. GDT + IDT + exception handlers — DONE
**Proves:** visibility into faults — everything after this depends on being
able to see *why* something broke instead of triple-faulting silently.
**Deliverables:**
- `kernel/arch/x86_64/gdt.c/.h`, `gdt_flush.asm`: C-managed 3-entry flat
  GDT (null/kernel-code/kernel-data), replacing boot.asm's throwaway one.
- `kernel/arch/x86_64/idt.c/.h`: 256-entry IDT, vectors 0-31 (CPU
  exceptions) populated as 64-bit interrupt gates; 32+ left not-present
  until Milestone 5 (IRQs).
- `kernel/arch/x86_64/isr.asm`: one stub per exception vector (0-31),
  normalizing to a uniform trap-frame shape and handling the long-mode
  stack-alignment gotcha explicitly (see ADR 0002).
- `kernel/arch/x86_64/exceptions.c`, `trap_frame.h`: `isr_handler` dumps
  vector/name/error-code/CR2(on #PF)/all registers to serial, then halts
  (no recovery path exists yet — every exception is fatal).
- `libk/fmt.c/.h`: `u64_to_hex`, host-tested (`tests/host/test_fmt.c`).
- `kernel/kernel.c`: calls `gdt_init()`/`idt_init()`, then deliberately
  triggers `int3` as a self-test that the fault-dump path works end to end.
**Verification:** `make run` boots the real ISO and prints `[OK] hello
kernel` → `[OK] gdt/idt installed` → the `int3` self-test's fault dump
(`#BP Breakpoint`, vector `0x3`, `cs=0x8`/`ss=0x10` matching the GDT
exactly, sane `rip`/`rsp`/all GPRs). `tests/qemu/test_boot_serial.sh` and
`tests/qemu/test_idt_selftest.sh` both pass. See ADR 0002 for the full
verification trail (object/link-level checks done before the toolchain
existed, plus what the live boot then confirmed).
**Design record:** `docs/adr/0002-gdt-idt-exception-handling.md`.
**Known limitation (accepted for this milestone only):** no TSS, no IST
stacks — a fault while the kernel stack itself is corrupt (e.g. stack
overflow) will double-fault onto the same bad stack rather than a
dedicated safe one. Revisit if/when that's actually observed, or
alongside the scheduler once interrupts can land on arbitrary thread
stacks.

## 3. Physical frame allocator — DONE
**Proves:** the kernel can actually discover and hand out real physical
memory, one 4KiB frame at a time — everything Milestone 4 builds (page
tables, kernel heap) needs frames to come from somewhere real.
**Deliverables:**
- `kernel/arch/x86_64/multiboot2.h`: Multiboot2 info-structure tag/mmap-
  entry layout, verified against GRUB's own header (see ADR 0003).
- `kernel/mm/pmm.c/.h`: bitmap frame allocator (128KiB bitmap, 4GiB
  tracking limit), parses the real memory map via `mbi_addr` (received
  since Milestone 1, unused until now), default-deny then carves out
  reserved ranges (frame 0, kernel image, multiboot info structure).
- `boot/linker.ld`, `kernel/arch/x86_64/boot.asm`: `__bss_start`/
  `__bss_end` + an explicit `.bss` zero before `call kernel_main` — a
  gap that predated this milestone (see ADR 0003) but only became
  load-bearing once a subsystem's correctness actually depended on it.
- `kernel/kernel.c`: calls `pmm_init(mbi_addr)`, prints the free/total
  frame counts, then an alloc→alloc→free→realloc self-test.
**Verification:** `make run` boots the real ISO and prints `[OK] pmm
initialized, free frames: 0x7eaf / total: 0x100000` (126.7MiB free,
4GiB tracked — both sanity-checkable, not just present) then `[OK] pmm
self-test passed (alloc/free/reuse)`. `tests/qemu/test_pmm_selftest.sh`
(new) plus the Milestone 1/2 smoke tests all re-verified passing after
the `.bss` fix. See ADR 0003 for the full trail.
**Design record:** `docs/adr/0003-physical-frame-allocator.md`.
**Known limitation (accepted for this milestone only):** frames above
4GiB physical are never tracked/allocatable (fixed-size bitmap, no heap
yet to size one dynamically) — revisit only if a real target's RAM ever
approaches that limit.

## 4. Paging/VMM + kernel heap — DONE
**Proves:** the kernel can turn a physical frame into usable memory at an
arbitrary virtual address, and allocate/free heap memory dynamically —
removes the "no malloc/free" constraint every prior milestone worked
around with static arrays.
**Deliverables:**
- `kernel/mm/vmm.c/.h`: 4-level page-table walker/mapper
  (`vmm_map_page`/`vmm_unmap_page`), extending boot.asm's live PML4
  (reused via `CR3`, never reloaded) with a new dedicated 1GiB region
  for the heap rather than touching the kernel image's own boot-time
  2MiB mapping. `invlpg` after every map/unmap.
- `libk/heap_alloc.c/.h`: first-fit free-list allocator with splitting/
  coalescing, pure hardware-free logic, host-tested
  (`tests/host/test_heap_alloc.c`, ASan/UBSan, 5 checks).
- `kernel/mm/heap.c/.h`: `kmalloc`/`kfree`, backed by a 1MiB region
  `heap_init()` eagerly maps via the VMM.
- `kernel/panic.c/.h`: shared `panic()`, factored out of `kernel_main`
  now that `vmm.c`/`heap.c` need it too (three real call sites, not
  speculative infrastructure).
- `kernel/kernel.c`: calls `heap_init()`, then an alloc→write→verify→
  free→reuse self-test.
**Verification:** `tests/host/test_heap_alloc.c` passes (5 checks,
ASan/UBSan). `make run` boots and prints `[OK] kernel heap initialized`
→ `[OK] heap self-test passed (alloc/write/verify/free/reuse)`.
`tests/qemu/test_heap_selftest.sh` (new) plus all three earlier
milestones' smoke tests re-verified passing. See ADR 0004 for the full
trail, including the identity-window invariant `vmm.c` checks at
runtime rather than assumes.
**Design record:** `docs/adr/0004-vmm-and-kernel-heap.md`.
**Known limitation (accepted for this milestone only):** no general
physical-memory direct-map — only the low 8MiB boot.asm identity-maps
is directly writable for new page-table bootstrap frames (checked at
runtime, panics if violated); heap is a fixed 1MiB with no growth-on-
demand yet. Revisit either only when something actually needs more
(a bigger heap, or mapping arbitrary physical memory like a device's
MMIO region).

## 5. PIT/APIC timer + IRQ handling — DONE
**Proves:** the kernel can be interrupted asynchronously by hardware on
its own schedule, not just run synchronously on demand — the foundation
Milestone 6's preemptive scheduler needs.
**Deliverables:**
- `kernel/drivers/pic.c/.h`: 8259 PIC remap (IRQ0-15 → vectors 32-47,
  ports/ICW values verified against Linux's own i8259 source), EOI,
  per-line mask/unmask. Legacy PIC+PIT chosen over APIC — confirmed with
  the user (roadmap said "PIT/APIC," ambiguous by design) — since APIC
  needs either ACPI MADT parsing (CLAUDE.md non-goal territory) or a
  hardcoded IOAPIC address, for benefits (per-CPU timers/IRQ routing)
  that don't matter without SMP (also a non-goal here). See ADR 0005.
- `kernel/drivers/pit.c/.h`: PIT channel 0, mode 3, programmable
  frequency (divisor verified against Linux's `PIT_TICK_RATE`); owns its
  own IRQ0 handler and a lock-free `volatile` tick counter (justified:
  single writer, single simple-load reader, atomic on x86_64).
- `kernel/arch/x86_64/irq.asm`, `common_stub.inc`: one stub per IRQ
  vector, sharing `isr.asm`'s exact save/align/restore sequence via a
  new shared NASM macro rather than duplicating it or hardcoding a
  struct-offset dispatch trick.
- `kernel/arch/x86_64/irq_dispatch.c/.h`: per-line handler registration
  and dispatch, called from `irq_common_stub`.
- `kernel/arch/x86_64/idt.c`: extended to also install IRQ gates
  (32-47) alongside the existing exception gates (0-31).
- `kernel/arch/x86_64/exceptions.c`: `isr_handler` now resumes normally
  for `#BP` specifically (traps are resumable by design) instead of
  halting like every other exception — lets the Milestone 2 self-test
  coexist with this milestone's requirement that the kernel keep
  running after boot.
- `kernel/kernel.c`: remaps the PIC, starts the PIT at 100Hz, unmasks
  IRQ0, enables interrupts, waits for 100 real ticks, then idles
  forever — the first milestone that doesn't end in a deliberate panic.
**Verification:** `make run` boots and prints the `#BP` fault dump
*followed by* (not ending in) `[OK] pic/pit initialized, timer IRQ0
unmasked` then `[OK] timer self-test passed (0x64 ticks received via
IRQ0)`. `tests/qemu/test_timer_irq_selftest.sh` (new) checks both
markers and that the `#BP` dump precedes the pic/pit line in the actual
output (proving resume-on-#BP works, not just present markers).
Milestones 1-4's smoke tests all re-verified passing.
**Design record:** `docs/adr/0005-pic-pit-irq-handling.md`.
**Known limitation (accepted for this milestone only):** only IRQ0 has
a registered handler — no keyboard (IRQ1) or other device driver yet;
`irq_register_handler` supports exactly one handler per line, no
chaining (nothing needs to share a line yet).

## 6. Preemptive scheduler + context switch (single CPU) — DONE
**Proves:** the kernel can forcibly preempt a running task and resume a
different one, on the timer's schedule, not the task's own — the
foundation Milestone 7's process model needs.
**Deliverables:**
- `kernel/sched/task.c/.h`: `task_t` (rsp, next, id) and `task_create()`,
  which builds a synthetic `trap_frame_t` on a fresh 16KiB kmalloc'd
  stack so a never-yet-run task can be resumed through the exact same
  `iretq` path a real interrupt uses.
- `kernel/sched/scheduler.c/.h`: round-robin ready queue (circular
  linked list), owns IRQ0 (calls `pit_tick()` itself), `scheduler_init`/
  `scheduler_add_task`.
- `kernel/arch/x86_64/common_stub.inc`: changed so `isr_handler`/
  `irq_handler` return the `trap_frame_t*` to actually resume, instead
  of the stub always restoring the one it saved — this is what makes a
  context switch possible with no separate save/restore mechanism.
  `isr_handler` always returns the same frame it was given (exceptions
  never switch tasks); only the scheduler's IRQ0 handler can return a
  different one.
- `kernel/drivers/pit.c/.h`: no longer self-registers an IRQ0 handler;
  now a plain hardware driver (`pit_init`/`pit_tick`/`pit_get_ticks`)
  the scheduler depends on, not the other way around.
- `kernel/kernel.c`: two demo kernel threads that never voluntarily
  yield (`for(;;) counter++`, no `hlt`), proving *forced* preemption
  specifically, not just cooperative switching.
**Verification:** `make run` boots and prints (after all Milestones
1-5 markers, unchanged) `[OK] scheduler initialized, 2 demo tasks
created` then `[OK] scheduler self-test passed, task A: 0x8b77079, task
B: 0x8812a58 (both made progress under preemption)` — ~146M vs ~143M
increments, within 2.4% of each other, itself evidence of *fair*
round-robin, not just "both nonzero." `tests/qemu/
test_scheduler_selftest.sh` (new) independently verifies both counters
are nonzero from the real output. All five earlier milestones' smoke
tests re-verified passing, confirming the `common_stub.inc` signature
change didn't regress exception/IRQ handling.
**Design record:** `docs/adr/0006-preemptive-scheduler.md`.
**Known limitation (accepted for this milestone only):** kernel threads
only, single shared address space (no ring 3 — Milestone 7); no
priorities or blocking (every task is always "in the rotation"); fixed
16KiB stack per task, no guard page or overflow detection yet.

## 7. Userspace: ring 3, syscalls, process model — DONE
**Proves:** the kernel can run code at ring 3 with genuine privilege
separation (can't execute privileged instructions or touch supervisor-
only memory) and service syscalls from it safely, including validating
user-supplied pointers before trusting them.
**Deliverables:**
- `kernel/arch/x86_64/gdt.c/.h`: extended with user code/data segments
  and a TSS descriptor, in the specific order SYSRET's `+8`/`+16`
  arithmetic requires (verified against Linux's `syscall_init()`).
- `kernel/arch/x86_64/tss.c/.h`: 64-bit TSS (byte-exact layout verified
  against Linux's `struct x86_hw_tss`), `tss_set_rsp0()` for per-task
  kernel stacks.
- `kernel/arch/x86_64/syscall.c/.h`, `syscall_entry.asm`: STAR/LSTAR/
  SFMASK MSR programming, `EFER.SCE`, the SYSCALL/SYSRET entry stub
  (can't reuse `common_stub.inc` — no IDT frame, manual stack switch,
  `sysretq` not `iretq`), `sys_nop`/`sys_write` (the latter validated
  via `vmm_is_user_range` before dereferencing anything).
- `kernel/mm/vmm.c/.h`: `VMM_FLAG_USER`, U-bit propagation through
  intermediate page-table levels (including upgrading pre-existing
  ones — see ADR 0007 for the real bug this fixed), `vmm_is_user_range`.
- `kernel/sched/task.c`, `user_demo.asm`: `task_create_user()` and the
  ring-3 demo program — hand-written position-independent NASM, not
  compiled C, since a normal C function lives inside the kernel's own
  supervisor-only 2MiB pages (ADR 0007 explains why that rules it out).
  Each ring-3 task gets its own dedicated kernel stack (a real
  correctness requirement, not a nicety — worked out by hand before
  writing code, see ADR 0007).
**Verification:** `make run` boots and prints, after all Milestones 1-6
markers unchanged: `[OK] tss/syscall initialized` → `[OK] scheduler
initialized, 2 kernel + 1 ring-3 demo task created` → `[OK] hello from
ring 3 via syscall` (the validated `sys_write`) → `[OK] syscall
self-test passed, 0x223628 syscalls serviced from ring 3` (millions of
`sys_nop` round-trips). `tests/qemu/test_ring3_syscall_selftest.sh`
(new) verifies real sequencing, not just marker presence. All six
earlier milestones' smoke tests re-verified passing. The first real
boot attempt actually faulted (`#PF` at the ring-3 entry point,
diagnosed via Milestone 2's own fault-dump infrastructure) before the
fix — see ADR 0007's Verification section for the full story.
**Design record:** `docs/adr/0007-ring3-syscalls-process-model.md`.
**Known limitation (accepted for this milestone only):** one embedded
demo task, shared address space — no fork/exec, no per-process page
tables, no filesystem-loaded programs (no FS exists). No NX enforcement
on data pages. Syscalls are non-preemptible (always resume the exact
context that made them). Revisit alongside a real FS/ELF loader.

## 8. VGA console, PS/2 keyboard, interactive shell — DONE
Redefined from the original "FS, drivers, SMP" placeholder: the user's
actual next priority was making the kernel observable and usable on
real hardware, not virtual-machine-only — every milestone through 7
only ever produced output on COM1 serial, which most real PCs
(especially laptops) don't have. FS and SMP are still not done; they're
pushed to Milestone 9+ (see below), not abandoned.
**Proves:** the kernel is observable and interactively usable without a
serial cable — on-screen output, real keyboard input, and something to
actually do once it boots.
**Deliverables:**
- `kernel/drivers/vga.c/.h`: legacy VGA text-mode console (0xB8000, 80x25,
  hardware cursor tracking, scrolling). Works via legacy BIOS/CSM boot;
  known limitation on UEFI-only boot with no CSM (ADR 0008).
- `kernel/drivers/console.c/.h`: fans every write out to both serial
  (keeps all seven earlier smoke tests working unchanged) and VGA.
  `kernel_main`/`panic.c`/`exceptions.c`/ring-3's `sys_write` all
  migrated from `serial_write`/`serial_putc` to `console_write`/
  `console_putc`, so panics and self-tests are visible on a real screen.
- `kernel/drivers/keyboard.c/.h`: PS/2 keyboard, IRQ1, Scancode Set 1,
  US QWERTY, basic Shift support. Scancode table is kernel-only; the
  producer/consumer queue is `libk/ring_buffer.c` (host-tested).
- `kernel/shell.c/.h`: `help`/`echo`/`uptime`/`clear`, replacing
  `kernel_main`'s trailing idle loop — the shell is just the bootstrap
  task's new steady-state activity, no new scheduling concept needed.
**Verification:** checked an assumption before starting (the ISO
already supports UEFI boot via `x86_64-efi` GRUB modules — confirmed
with `xorriso -report_el_torito`, not assumed). `make run` boots with
serial output unchanged. VGA output verified visually via QEMU
`screendump` screenshots (not just text), matching serial exactly,
including the full `#BP` dump. Real keyboard input verified via QEMU
monitor `sendkey` (actual virtual PS/2 controller, not a shortcut):
typed `help` and `echo hello world` into a live boot, both correctly
read/echoed/executed. `tests/qemu/test_shell_selftest.sh` (new)
automates this with real synchronization (waits for the shell-prompt
marker before sending keys, not a guessed delay) — run 3x with no
flakiness. `tests/host/test_ring_buffer.c` (new, 4 tests) passes. All
seven earlier milestones' smoke tests and all three host tests
re-verified passing.
**Design record:** `docs/adr/0008-vga-keyboard-shell.md`.
**Known limitation (accepted for this milestone only):** VGA text mode
doesn't work on UEFI-only boot without CSM (no framebuffer console
yet); keyboard has no Caps Lock/Ctrl/Alt/function-key/numpad support;
shell has no scripting, variables, or piping.

## 9. Per-process address spaces — DONE
First step of the "build this into a real OS" inventory worked through
with the user after Milestone 8 (process model, storage/FS, drivers,
memory maturity, synchronization/IPC) — taken one step at a time, per
the user's explicit instruction, starting with this one since everything
else on that list depends on genuine process isolation existing first.
**Proves:** two ring-3 processes get independent top-level page tables
(not a shared address space, as every task through Milestone 8 had) and
both still work correctly — including making validated syscalls —
without either being able to see or corrupt the other's memory.
**Deliverables:**
- `kernel/mm/vmm.c/.h`: `vmm_map_page_in(pml4_phys, ...)` (map into an
  explicit PML4, not just the currently-active one), `vmm_current_pml4()`,
  `vmm_create_address_space()` (allocates a fresh PML4, shares the
  kernel-half entry `PML4[511]` and the identity-map entry `PML4[0]` by
  reference with the caller's own table). `vmm_map_page()` is now defined
  in terms of `vmm_map_page_in()`.
- `kernel/arch/x86_64/common_stub.inc`: the scheduler's `CR3` reload
  moved into assembly, placed after `RSP` has already switched to the
  incoming task's own stack — doing this in C, before the stack switch,
  was the more serious of the two real bugs this milestone found (see
  ADR 0009).
- `kernel/sched/scheduler.c/.h`: `scheduler_current_pml4`/
  `scheduler_target_pml4` globals the assembly above reads/writes;
  `timer_tick_handler` just records the target, doesn't switch `CR3`
  itself anymore.
- `kernel/sched/task.h/.c`: `task_t` gained a `pml4` field.
  `task_create()` (kernel threads) records the kernel's own existing
  space; `task_create_user()` now calls `vmm_create_address_space()` and
  maps the demo program's code (shared read-only across every process)
  and a private stack into it. User-space code/stack relocated from
  `0x400000`/PML4 index 0 (Milestone 7) to `0x8000400000`/PML4 index 1,
  since index 0 is now committed to the shared identity map.
- `kernel/kernel.c`: creates two ring-3 processes instead of one,
  printing both their `PML4` physical addresses.
**Verification:** `make run` boots and prints, after all Milestones 1-8
markers unchanged: `[OK] process A pml4: 0x23b000, process B pml4:
0x244000 (different address spaces)` then **two** independent `[OK]
hello from ring 3 via syscall` lines, then all self-tests passing with
roughly double Milestone 7's syscall count. `tests/qemu/
test_process_isolation_selftest.sh` (new) asserts the two `PML4`
addresses actually differ and the hello message appears exactly twice —
not just "both ran." Two earlier smoke tests needed stale marker text
updated (2 processes instead of 1), not behavior fixes. All eight
earlier milestones' smoke tests and all three host tests re-verified
passing. The first two real boot attempts actually triple-faulted/
garbled respectively before the fixes — both root-caused via
`-d int,cpu_reset` traces and register-state reasoning, not guessed —
see ADR 0009's Verification section for the full diagnostic trail.
**Design record:** `docs/adr/0009-per-process-address-spaces.md`.
**Known limitation (accepted for this milestone only):** still no
ELF loader (the one demo program is embedded at link time, not loaded
from a filesystem), no `sys_exit`/process teardown, no `fork`/`exec`, no
demand paging or copy-on-write, no per-process resource limits. Revisit
alongside the ELF loader and process lifecycle work next.

## 10. Process lifecycle: sys_exit and teardown — DONE
Second step of the post-Milestone-8 "build this into an OS" inventory,
worked one step at a time per the user's instruction — the natural
follow-on to Milestone 9: processes could be created and isolated, but
never stopped.
**Proves:** a ring-3 process can call `sys_exit`, actually terminate,
and have every resource it held (its address space, its user and
kernel stacks, its task struct) reclaimed with zero leaks — not just
that `sys_exit` doesn't crash the kernel.
**Deliverables:**
- `kernel/arch/x86_64/syscall.h/.c`: `SYS_EXIT`, dispatched to
  `scheduler_exit_current()`.
- `kernel/sched/task.h`: `task_t` gained `state` (`TASK_READY`/
  `TASK_ZOMBIE`), `prev` (the ready queue is now doubly-linked, for
  O(1) zombie removal), and `kernel_stack_base` (needed to `kfree()` a
  reaped task's kernel stack correctly).
- `kernel/sched/scheduler.h/.c`: `scheduler_exit_current()` (marks the
  caller a zombie, re-enables interrupts, halts forever — reuses the
  existing preemption path rather than inventing a new switch
  mechanism); a dedicated reaper kernel thread (spawned internally by
  `scheduler_init()`) that actually frees a zombie's resources, on its
  own stack, only once it's safe to (see ADR 0010 for why
  `timer_tick_handler` itself can't do this synchronously).
- `kernel/mm/vmm.h/.c`: `VMM_FLAG_OWNED` (marks a leaf mapping's
  physical frame as this process's own, vs. shared/static memory it
  merely maps) and `vmm_destroy_address_space()`, which frees an
  entire process-private page-table tree while leaving the shared
  kernel-half/identity-map entries untouched.
- `kernel/sched/task.c`: the demo program's stack mapping gets
  `VMM_FLAG_OWNED`; its code mapping deliberately doesn't (shared, ADR
  0009).
- `kernel/sched/user_demo.asm`: bounded `sys_nop` loop (200,000
  iterations) followed by `sys_exit`, instead of looping forever.
- `kernel/kernel.c`: captures `pmm_frames_free()` before either process
  is created, waits for both to be reaped, and panics if the frame
  count hasn't returned to exactly that baseline.
**Verification:** `make run` boots and prints, after all Milestones
1-9 markers unchanged, both processes' syscall self-test messages,
then `[OK] process 4 exited and was reaped` / `[OK] process 5 exited
and was reaped`, then `[OK] process lifecycle self-test passed, ...
frames free, matches pre-creation baseline`. `tests/qemu/
test_process_lifecycle_selftest.sh` (new) verifies exactly two reap
messages, the self-test's pass line, and that the shell still starts
normally afterward. All nine earlier milestones' smoke tests and all
three host tests re-verified passing. Booted 4 times back to back with
identical output — unlike Milestone 9, this landed correctly on the
first real boot, since the design (reaper task on its own stack, never
freeing state you're still executing on) was worked out from ADR
0009's lesson before writing code, not discovered by crashing.
**Design record:** `docs/adr/0010-process-lifecycle-sys-exit.md`.
**Known limitation (accepted for this milestone only):** no parent/
`wait()`/exit-code-reporting mechanism (nothing has a parent-process
concept yet — every process is spawned directly by `kernel_main`); no
`fork`/`exec`; still no ELF loader (the one demo program is embedded at
link time). Revisit alongside a real process-creation syscall.

## 11. NX (no-execute) enforcement — DONE
Third step of the post-Milestone-8 "build this into an OS" inventory —
the memory-maturity item "NX enforcement on data pages," tackled next
since both writable regions that needed it (the kernel heap, a
process's stack) already existed.
**Proves:** the kernel heap and a ring-3 process's stack are genuinely
non-executable at the hardware level (W^X) — a buffer overflow or
stack-smashing attack landing bytes there can no longer be turned into
code execution by jumping into either.
**Deliverables:**
- `kernel/arch/x86_64/msr.h` (new): `read_msr`/`write_msr`, factored
  out of `syscall.c` once `vmm.c` needed the identical helpers for a
  second MSR.
- `kernel/mm/vmm.h/.c`: `VMM_FLAG_NX` (maps directly onto PTE bit 63);
  `vmm_enable_nx()` (checks CPUID `80000001h:EDX` bit 20, panics if
  unsupported, sets `EFER.NXE`); `vmm_page_is_executable_in()` (reads
  back real page-table state for verification).
- `kernel/mm/heap.c`, `kernel/sched/task.c`: the kernel heap and a
  process's stack mappings both gained `VMM_FLAG_NX`; the demo
  program's code mapping deliberately did not.
- `kernel/arch/x86_64/exceptions.c`: `#PF` dumps now decode the error
  code (present/write/user/reserved-bit/instruction-fetch) instead of
  only printing the raw hex value.
- `kernel/kernel.c`: calls `vmm_enable_nx()` early (before `pmm_init()`)
  and a self-test confirming a live heap pointer is reported non-
  executable while `kernel_main` itself is still reported executable.
**Verification:** `make run` boots and prints `[OK] NX (no-execute)
enabled` right after `[OK] gdt/idt installed`, then later `[OK] NX
self-test passed (heap is non-executable, kernel code still is)`, with
every Milestone 1-10 marker through the shell prompt unchanged
afterward. `tests/qemu/test_nx_selftest.sh` (new) verifies both markers
and that NX was enabled strictly before the heap was mapped. All ten
earlier smoke tests and all three host tests re-verified passing.
Booted 4 times back to back with identical output — correct on the
first real attempt, no flakiness, same as Milestone 10.
**Design record:** `docs/adr/0011-nx-enforcement.md` — including why
verification checks the resulting PTE bit directly rather than
triggering a live NX fault (no exception-recovery mechanism exists yet
to safely resume past one; flagged as a deliberately deferred, more
indirect form of proof, not smuggled past as equivalent).
**Known limitation (accepted for this milestone only):** the kernel
image itself (`.text`/`.rodata`/`.data`/`.bss`, boot.asm's coarse 2MiB
mappings) isn't NX-hardened at all — `.data`/`.bss` are still
executable in principle, since boot.asm maps the whole low 8MiB region
as one set of 2MiB pages with no per-section granularity. Splitting
that apart is a separate, larger paging change (already flagged as
future work back in ADR 0004), not part of this step. No exception-
recovery/signal-delivery mechanism exists yet, so a real NX violation
still just halts the kernel rather than terminating only the
offending process.

## 12. Kernel stack guard pages — DONE
Fourth step of the post-Milestone-8 "build this into an OS" inventory —
"guard pages," the last of the memory-maturity items named alongside
NX (Milestone 11) in that original list.
**Proves:** every kernel-mode stack (a kernel thread's whole stack, a
ring-3 process's separate kernel-mode stack) lives in its own
dedicated, page-mapped VA region with a genuinely unmapped guard page
immediately below it — a downward overflow now takes an immediate
`#PF` instead of silently corrupting an adjacent heap object, which is
what the previous `kmalloc()`-based stacks were exposed to.
**Deliverables:**
- `kernel/mm/vmm.h/.c`: `vmm_translate()` (general VA->PA lookup, used
  both by teardown and the new self-test).
- `kernel/sched/task.c`: `alloc_kernel_stack()`/
  `task_free_kernel_stack()`, a dedicated `PML4[511]:PDPT[509]` (
  `0xFFFFFFFF40000000`) region with a guard page before every stack
  slot, shared by both `task_create()` and `task_create_user()` instead
  of `kmalloc()`. Also documents that a process's USER-mode stack
  already had equivalent protection as a side effect of ADR 0009's
  sparse per-process address-space design.
- `kernel/sched/scheduler.c`: the reaper now calls
  `task_free_kernel_stack()` instead of `kfree()` to tear down a
  reaped process's kernel-mode stack.
- `kernel/kernel.c`: self-test confirming the page below a live kernel
  thread's stack is genuinely unmapped.
**Verification:** `make run` boots and prints `[OK] guard page
self-test passed (kernel stack guard page is unmapped)` right after
`[OK] tss/syscall initialized`, with every Milestone 1-11 marker
through the shell prompt unchanged afterward. A `-d int,cpu_reset`
trace across a full boot showed zero page/double faults (only the
deliberate `#BP`) and every timer-tick IRQ landing on `SP` values
inside the new dedicated region. `tests/qemu/
test_guard_page_selftest.sh` (new) verifies the marker and its
ordering. The Milestone 10 process-lifecycle leak check still passes
unmodified — now a strictly stronger check, since a process's kernel
stack consumes real `pmm` frames directly rather than being hidden
inside the heap's fixed footprint. All eleven earlier smoke tests and
all three host tests re-verified passing. Booted 4 times back to back
with identical output — correct on the first real attempt, no
flakiness, same as Milestones 10 and 11.
**Design record:** `docs/adr/0012-kernel-stack-guard-pages.md`.
**Known limitation (accepted for this milestone only):** the kernel
heap itself has no guard pages or per-allocation isolation (one
contiguous first-fit-managed region, unchanged); guard-page VA slots
are never reclaimed/reused even after a task is reaped (a monotonic
bump allocator, acceptable until something actually approaches
exhausting the dedicated 1GiB region, which nothing here does yet); no
exception-recovery/signal-delivery mechanism exists yet, so a real
stack overflow still halts the kernel rather than terminating only the
offending task.

## 13. PCI bus enumeration — DONE
Fifth step of the post-Milestone-8 "build this into an OS" inventory —
"PCI enumeration," named in the original hardware/drivers list.
Groundwork for any future driver beyond the fixed-port legacy devices
already supported (PIC/PIT/PS-2/VGA/serial): there's no way to find a
real device's I/O ports/BARs/IRQ without first knowing where it lives
on the bus.
**Proves:** the kernel can discover real hardware present on the PCI
bus via the legacy Configuration Mechanism #1 (`0xCF8`/`0xCFC`) — not
just that a scan function runs without crashing, but that it finds and
correctly decodes actual devices QEMU exposes.
**Deliverables:**
- `libk/io.h`: `outl`/`inl` (32-bit port I/O), needed for the first
  time by this driver.
- `kernel/drivers/pci.h/.c` (new): `pci_scan()` — brute-force scans
  all 256 buses x 32 devices x 8 functions via `CONFIG_ADDRESS`/
  `CONFIG_DATA`, reporting each present function's vendor/device ID and
  class/subclass/prog-if/header-type through a callback. Pure
  enumeration only — doesn't drive any device found.
- `kernel/kernel.c`: calls `pci_scan()`, logs every device found, and
  self-tests that QEMU's Intel host bridge (bus 0/device 0/function 0,
  vendor `0x8086`) specifically was found — the one device guaranteed
  present regardless of which other peripherals a given QEMU version/
  invocation exposes.
**Verification:** `make run` boots and prints six real `[PCI] ...`
lines (host bridge, ISA bridge, IDE controller, PM bridge, VGA, NIC —
QEMU's actual i440fx devices) then `[OK] pci self-test passed (0x6
device(s) found, host bridge present)`, with every Milestone 1-12
marker through the shell prompt unchanged afterward. `tests/qemu/
test_pci_selftest.sh` (new) verifies the exact host-bridge line and the
device count. All twelve earlier smoke tests and all three host tests
re-verified passing. Booted 4 times back to back with an identical
device list each time — correct on the first real attempt, no
flakiness, same as Milestones 10-12.
**Design record:** `docs/adr/0013-pci-enumeration.md`.
**Known limitation (accepted for this milestone only):** brute-force
scan of all 256 buses (no bridge-topology-aware recursive walk — fast
enough in practice for what's being scanned, deferred until it isn't);
no MMIO-based Enhanced Configuration Access Mechanism (would need ACPI
MCFG parsing, a listed non-goal pending confirmation); nothing found by
the scan is actually driven yet — enumeration only. Revisit alongside
whichever specific device (disk, NIC) a future storage/network step
actually needs.

## 14. CMOS RTC driver and a `date` command — DONE
Sixth step of the post-Milestone-8 "build this into an OS" inventory —
"RTC," named in the original hardware/drivers list, and one of the few
remaining items not adjacent to a flagged non-goal (storage/filesystem
and ACPI-based shutdown were flagged to the user rather than started
after Milestone 13 found a real IDE controller).
**Proves:** the kernel can read and correctly decode real wall-clock
time from the CMOS RTC (handling whatever BCD/binary and 12/24-hour
mode the hardware is actually configured in), both at boot and
on-demand from the shell.
**Deliverables:**
- `libk/fmt.h/.c`: `u32_to_dec()` — the kernel's first decimal
  formatter (previously hex-only), host-tested alongside the existing
  one.
- `kernel/drivers/rtc.h/.c` (new): `rtc_read()` — double-read-until-
  stable against the RTC's own update cycle (Status A's UIP bit),
  BCD->binary and 12->24-hour conversion handled internally so every
  caller always gets plain 24-hour binary fields regardless of hardware
  configuration.
- `kernel/kernel.c`: reads and range-validates the time at boot (no
  known-expected value exists to compare against, so the self-test
  checks every field is in a sane range instead).
- `kernel/shell.c`: new `date` command, printing `YYYY-MM-DD HH:MM:SS
  UTC (from CMOS RTC)`.
**Verification:** `make run` boots and prints `[OK] rtc self-test
passed, boot time ... year 0x7ea month 0x8 day 0x18 hour 0xd min 0x11
sec 0x1c` — decoding to the real date the boot ran on. The shell's
`date` command was exercised with real injected keystrokes (QEMU
monitor `sendkey`, extending `tests/qemu/test_shell_selftest.sh`,
which also needed its stale `help`-text assertion fixed).
`tests/qemu/test_rtc_selftest.sh` (new) independently range-checks the
boot-time year from real captured output. `tests/host/test_fmt.c`
gained six new checks for `u32_to_dec()`. All thirteen earlier smoke
tests and all three host tests re-verified passing. Booted 4 times back
to back with correctly-advancing output each run — correct on the
first real attempt, no flakiness, same as Milestones 10-13.
**Design record:** `docs/adr/0014-rtc-driver.md`.
**Known limitation (accepted for this milestone only):** no IRQ8-driven
update-ended interrupt (busy-wait/double-read instead — fine for an
occasionally-read clock, would need revisiting for a settable/
continuously-ticking system clock); no CMOS century register read
(21st century assumed); date is read-only, nothing can set it.

## 15. Legacy (non-ACPI) reboot — DONE
Seventh step of the post-Milestone-8 "build this into an OS" inventory
— "shutdown/reboot," named in the original hardware/drivers list. Full
ACPI shutdown needs ACPI table parsing (flagged, awaiting the user's
call); a system RESET doesn't need ACPI at all, so this covers the
"reboot" half now.
**Proves:** the shell's `reboot` command triggers a genuine CPU reset
via the legacy 8042 keyboard-controller mechanism, not just that a
function was called without crashing.
**Deliverables:**
- `kernel/arch/x86_64/reboot.h/.c` (new): `reboot()` — pulses the 8042
  controller's reset line (port `0x64`, command `0xFE`); falls back to
  an intentional triple fault (deliberately invalid IDTR + `int3`) if
  the controller doesn't respond.
- `kernel/shell.c`: new `reboot` command.
**Verification:** booted without `-no-reboot`, typed `reboot` via real
injected keystrokes, and the entire Milestone 1-14 boot sequence
printed a second time — direct proof of a genuine reset. Found and
fixed a real QEMU flag interaction while building the smoke test:
`-no-shutdown` (used by every other test in this suite) overrides
`-no-reboot`'s "exit instead of rebooting" behavior, causing QEMU to
hang rather than exit after a real reset — diagnosed by testing flag
combinations individually, not guessed. `tests/qemu/
test_reboot_selftest.sh` (new) deliberately omits `-no-shutdown` and
asserts QEMU exits promptly after the reboot command, unlike a hung
kernel which would run to the full timeout. `test_shell_selftest.sh`
needed its `help`-text assertion updated (stale-marker fix, not a
regression). All fourteen earlier smoke tests and all three host tests
re-verified passing. Correct on the first real attempt.
**Design record:** `docs/adr/0015-legacy-reboot.md`.
**Known limitation (accepted for this milestone only):** no power-off
(reset only) — ACPI-based shutdown remains flagged, awaiting the
user's decision on the ACPI non-goal.

## 16. PS/2 mouse driver — DONE
Eighth step of the post-Milestone-8 "build this into an OS" inventory —
"mouse," the last hardware item from the original list not adjacent to
a flagged non-goal. No graphics/framebuffer exists yet (VGA text mode
only, ADR 0008), so there's no cursor to draw — this proves the PS/2
protocol can be decoded correctly, deferring rendering to a future
graphics milestone.
**Proves:** real PS/2 mouse movement and button packets, injected as
actual synthetic hardware events (not a shortcut around the driver),
are correctly decoded — not just that `mouse_init()` runs without
crashing.
**Deliverables:**
- `kernel/drivers/mouse.h/.c` (new): `mouse_init()`/`mouse_has_event()`/
  `mouse_get_event()` — 8042 auxiliary-port setup, IRQ12 registration,
  standard 3-byte streaming packet decode into a small fixed-capacity
  event queue.
- `kernel/kernel.c`: unmasks IRQ2 (the master PIC's cascade line —
  required for ANY slave-PIC line, including IRQ12, to ever reach the
  CPU) alongside IRQ12.
- `kernel/shell.c`: new `mouse` command.
**Verification:** `make run` boots and prints `[OK] pic/pit/keyboard/
mouse initialized, IRQ0+IRQ1+IRQ2+IRQ12 unmasked`, confirmed against
live QEMU-monitor `info pic` register state, not just the kernel's own
claim. **A real bug was found and fixed during verification** — the
first of this session's eight milestones to hit one on a live boot: a
stray device ACK byte (`0xFA`) could arrive asynchronously and get
mistaken for a legitimate packet-start byte (its bit 3 coincidentally
matches the framing spec's resync bit), corrupting the first real
packet and permanently desyncing the parser. Diagnosed via temporary
per-byte instrumentation (not guessed), fixed with an explicit
ACK/RESEND byte-value check in the packet parser. Confirmed fixed by
rerunning the exact failing scenario. `tests/qemu/
test_mouse_selftest.sh` (new) injects real synthetic movement and
button events via the QEMU monitor and asserts the shell decoded them
correctly, with real polling-based step synchronization (a timing bug
in an earlier draft of the test itself produced a separate, misleading
intermediate failure during development). `test_shell_selftest.sh` and
`test_timer_irq_selftest.sh` needed stale marker text updated. All
fifteen earlier smoke tests and all three host tests re-verified
passing.
**Design record:** `docs/adr/0016-ps2-mouse-driver.md` — includes the
full diagnostic trail for the ACK-byte bug.
**Known limitation (accepted for this milestone only):** no
cursor/display integration (nothing to draw one on yet); `dx`/`dy` are
reported in raw PS/2 wire convention (positive `dy` = up), left
unconverted since there's no display consumer yet to define a screen
convention; no scroll-wheel (`IntelliMouse`) support.

## 17. ELF64 loader for ring-3 processes — DONE
First of `future.md`'s "reasonable next steps" (not adjacent to any
flagged non-goal): every ring-3 process through Milestone 16 ran the
exact same hand-written raw code blob (`user_demo.asm`) — not a real
loader, a single hardcoded program.
**Proves:** the kernel can parse and correctly map a REAL, multi-segment
ELF64 static executable into a process's own address space — distinct
per-segment permissions derived from the file's own program headers
(real W^X, not one fixed policy), `.bss` genuinely zero-filled, `.data`
genuinely copied from the file — not just that a ring-3 program runs.
**Deliverables:**
- `libk/elf.h/.c` (new): ELF64 header/program-header structs and
  bounds/overflow-checked validation (`elf64_validate`,
  `elf64_get_phdr`, `elf64_validate_load_segment`) — pure parsing, no
  kernel dependency, host-tested (`tests/host/test_elf.c`, 10 checks).
- `kernel/mm/elf_loader.h/.c` (new): `elf_load()` — walks validated
  `PT_LOAD` segments, allocates fresh `VMM_FLAG_OWNED` frames per
  segment (zeroed first, then file bytes copied in — how `.bss` comes
  out zero-filled for free), maps them with permissions derived from
  each segment's own `p_flags`.
- `kernel/mm/vmm.h`: `VMM_IDENTITY_WINDOW_LIMIT` moved here from
  `vmm.c` (now a second caller needs it).
- `kernel/user/hello.asm`, `kernel/user/user.ld` (new): a real
  standalone ELF64 executable — text/rodata/data/bss, `.data`/`.bss`
  self-verified at runtime — linked as a genuinely separate build
  (`x86_64-elf-ld`, no kernel flags), replacing `user_demo.asm`
  (retired/removed).
- `kernel/sched/user_elf_blob.asm` (new): embeds the compiled
  `build/kernel/user/hello.elf` into the kernel image via `incbin`.
- `kernel/sched/task.c`: `task_create_user()` now calls `elf_load()`
  instead of hand-mapping a shared demo code page; the process's entry
  RIP comes from the ELF's own `e_entry`.
**Verification:** `make run` boots and prints, after all Milestones
1-16 markers unchanged, `[OK] hello from ring 3 via ELF-loaded process`
then `[OK] elf .data/.bss segment verification passed` — **twice**,
once per independently-loaded process — then the existing
syscall/lifecycle self-tests passing with zero frame leak (proving the
now-private, per-process segment frames are correctly reclaimed).
`tests/qemu/test_elf_loader_selftest.sh` (new) independently checks the
verification message appears exactly twice and its `[FAIL]` counterpart
never appears. `test_ring3_syscall_selftest.sh` and
`test_process_isolation_selftest.sh` needed stale marker text updated
(the demo program's message changed), not behavior fixes. All sixteen
earlier smoke tests and all three pre-existing host test suites
re-verified passing. Booted 4 times back to back with identical
output — correct on the first real attempt, no flakiness.
**Design record:** `docs/adr/0017-elf-loader.md`.
**Known limitation (accepted for this milestone only):** no dynamic
linking, no sub-page-aligned `PT_LOAD` segment support (rejected
outright rather than mis-handled), no shared/copy-on-write text pages
(every process gets a fresh private copy of every segment — a
deliberate regression from the old shared-demo-page design, accepted
pending a real COW milestone), still no filesystem-loaded programs (the
ELF image is embedded/statically linked at build time), no
`fork`/`exec` (every process is still spawned directly by
`kernel_main`).

## 18. `sys_fork`, non-blocking `sys_wait`, and exit codes — DONE
Second of `future.md`'s "reasonable next steps": Milestone 10 built
`sys_exit`/teardown but explicitly deferred `fork`/`exec`-equivalent
syscalls and a parent/child relationship, since nothing had a parent
process concept yet. Milestone 17's ELF loader made `exec` meaningful;
`fork` is the more foundational half of that pair, so it came first.
**Proves:** a ring-3 process can fork into a genuinely independent
child (its own deep-copied address space, not aliasing the parent), and
the parent's non-blocking `sys_wait` correctly observes exactly the
exit code the child passed to `sys_exit` — not just that neither
syscall crashes the kernel.
**Deliverables:**
- `kernel/mm/vmm.h/.c`: `vmm_for_each_user_page()` — read-only
  enumeration of every present leaf mapping in a process's private
  region, reusing `vmm_destroy_address_space()`'s traversal shape.
- `kernel/arch/x86_64/syscall_entry.asm`: `saved_user_rsp` promoted
  from file-local to `global`, exposing the user RSP at syscall time to
  C (`syscall_get_user_rsp()`) — needed to build a forked child's
  resume context.
- `kernel/arch/x86_64/syscall.h/.c`: `SYS_FORK`/`SYS_WAIT`; `sys_fork()`
  (wraps `task_fork()`); `sys_wait()` (non-blocking, writes the exit
  code to a validated user pointer); `sys_exit`'s calling convention
  extended with an exit-code argument (`rdi`).
- `kernel/sched/task.h/.c`: `task_t` gained `parent_id`/`exit_code`;
  `task_create_user()` split into a thin wrapper over the new
  `task_create_user_image()` (any embedded image, not just `hello.elf`);
  `task_fork()` — deep-copies the parent's address space
  page-by-page and builds the child's synthetic resume `trap_frame_t`
  from the parent's in-flight `syscall_frame_t`, with `rax` forced to 0.
- `kernel/sched/scheduler.h/.c`: `scheduler_exit_current()` now takes
  an exit code; `scheduler_current_task()`; `scheduler_try_wait()` and
  a new `collected_head` chain (reaped-but-unwaited-for children,
  cli/sti-protected on both sides); the reaper only immediately
  `kfree()`s an orphan's `task_t` now, deferring a parented one.
- `kernel/user/fork_demo.asm` (new), `kernel/sched/fork_demo_blob.asm`
  (new): a real ring-3 program that forks once, verifies its child's
  exit code via a poll loop over `sys_wait`.
- `kernel/kernel.c`: spawns the fork/wait demo as a third orphan
  process alongside the two `hello` ones; the frame-leak self-test's
  reap threshold raised from 2 to 4.
**Verification:** `make run` boots and prints, after all Milestones
1-17 markers unchanged, the fork/wait demo's creation line, the
child's own message, the parent's exit-code-verified message, four
"exited and was reaped" lines, then the existing self-tests passing
with the process-lifecycle self-test's frame count matching the
pre-creation baseline EXACTLY (proving the child's private deep-copied
memory came back too, not just the two hello processes').
`tests/qemu/test_fork_wait_selftest.sh` (new) independently checks real
sequencing, the exact verification message, absence of its `[FAIL]`
counterpart, and the four-reaps/leak-free combination.
`test_process_lifecycle_selftest.sh` needed its exact reaped-count
assertion updated from 2 to 4 (scope growth, not a behavior fix). All
eighteen earlier smoke tests and all three pre-existing host test
suites re-verified passing. Booted 4 times back to back with identical
shape each time — correct on the first real attempt, no flakiness.
**Design record:** `docs/adr/0018-fork-wait-exit-codes.md`.
**Known limitation (accepted for this milestone only):** no
copy-on-write (every fork is a full eager deep copy); no blocking
`wait()` (poll-based only — this kernel's syscalls are still
non-preemptible with interrupts masked throughout, ADR 0007); no
`exec`-equivalent syscall yet (a forked child always runs its parent's
exact image); no reparenting of orphaned children (an exited parent's
never-`wait()`-ed-for child's `task_t` struct would leak, though its
memory is still reclaimed unconditionally by the reaper); no process
groups, signals, or `SIGCHLD`-equivalent notification.

## 19. General physical-memory direct-map — DONE
Third of `future.md`'s "reasonable next steps": by Milestone 18, three
separate subsystems (`vmm.c`'s own page-table bootstrap, Milestone 17's
ELF loader, Milestone 18's `task_fork()`) independently depended on the
same `VMM_IDENTITY_WINDOW_LIMIT` constraint ADR 0004 flagged as a known
limitation to revisit "only when something actually needs more" — two
real subsystems hitting it was that trigger.
**Proves:** any physical frame `pmm_alloc_frame()` hands out — not just
one inside the low 8MiB identity window — is directly writable via a
new general-purpose translation, and the two subsystems that most
needed this (the ELF loader, fork's page copy) work correctly through
it with no regression.
**Deliverables:**
- `kernel/mm/vmm.h/.c`: `vmm_direct_map_init()` — maps the full 4GiB
  `pmm.h` tracks at a fixed virtual base (`PDPT[505..508]`, verified
  free via `python3`, under the SAME shared `PML4[511]` entry every
  other kernel-half region already lives under, so it's automatically
  visible from kernel code no matter which process's `CR3` is active)
  using 2MiB `PTE_PS` pages, the same encoding `boot.asm`'s own
  identity map already proved correct. `vmm_phys_to_virt()` — the
  actual translation. Narrows (doesn't remove) the doc comment on
  `VMM_IDENTITY_WINDOW_LIMIT`: it still applies to `vmm.c`'s own
  page-table bootstrap frames (irreducible — they build the tables the
  direct-map itself depends on) but no longer to DATA frames.
- `kernel/mm/elf_loader.c`, `kernel/sched/task.c`: `elf_load()`'s
  segment-destination writes and `task_fork()`'s `fork_copy_page()`
  both switched from a raw identity-window-constrained pointer cast to
  `vmm_phys_to_virt()`; both `>= VMM_IDENTITY_WINDOW_LIMIT` panic
  checks removed (no longer a reachable failure mode for them).
- `kernel/kernel.c`: calls `vmm_direct_map_init()` right after
  `pmm_init()` — before `heap_init()` or any process/fork ever runs, an
  ordering requirement (the direct-map's PDPT entries must already
  exist the first time `PML4[511]` gets copied by reference into a new
  process's table, ADR 0009) — then a self-test cross-checking a write
  through `vmm_phys_to_virt()` against a read through the completely
  independent low identity mapping.
**Verification:** `make run` boots and prints `[OK] physical memory
direct-map initialized` then `[OK] direct-map self-test passed (write
via vmm_phys_to_virt visible via the low identity mapping)`, strictly
before `[OK] kernel heap initialized`, with every Milestone 1-18 marker
through the shell prompt unchanged afterward — including both
downstream consumers' own self-tests (ELF `.data`/`.bss` verification,
fork/wait exit-code verification), now exercising the new code path
with no regression. `-d int,cpu_reset` trace across a full boot (per
CLAUDE.md's explicit prescription for paging changes) showed zero
`#PF`/double-fault/reset events, only the deliberate `#BP`.
`tests/qemu/test_direct_map_selftest.sh` (new) independently checks
both markers, their ordering relative to heap init, and that the
downstream self-tests still pass. All eighteen earlier smoke tests and
all four host test suites re-verified passing — the first milestone
since Milestone 8 that needed ZERO marker-text updates to any existing
test. Booted 4 times back to back with identical shape each time —
correct on the first real attempt, no flakiness.
**Design record:** `docs/adr/0019-physical-direct-map.md`.
**Known limitation (accepted for this milestone only):** fixed 4GiB
coverage, matching `pmm.h`'s own bitmap tracking limit exactly (not an
independent new limitation — ADR 0003 already capped tracking there).
`vmm_phys_to_virt()` doesn't validate its input is an actual
`pmm_alloc_frame()`-issued frame (same trust boundary as every other
raw-physical-address consumer in this codebase). `vmm.c`'s own
page-table bootstrap frames still need `VMM_IDENTITY_WINDOW_LIMIT` —
irreducible, not an oversight.

## 20. Genuinely blocking sys_wait() — DONE
Motivated by Milestone 18's own named limitation (ADR 0018): `sys_wait`
was non-blocking specifically because this kernel's syscalls ran fully
non-preemptible (interrupts masked throughout, ADR 0007) — a real
blocking wait needed a way to sleep with interrupts enabled and resume
later, which didn't exist yet.
**Proves:** a single `sys_wait` call from ring 3 now genuinely blocks
until a matching child exits — no userspace poll loop needed anymore
— and this was verified as ACTUALLY exercised (not just correct by
luck), via a new self-test that panics if the blocking path was never
taken.
**Deliverables:**
- `kernel/arch/x86_64/syscall.c`: `sys_wait()` rewritten as a loop —
  `scheduler_try_wait()`, and if nothing matches yet, `sti; hlt; cli`
  and retry — relying entirely on the ALREADY-EXISTING preemptive
  round-robin scheduler to give other tasks (including the reaper) a
  turn in between. No new `TASK_BLOCKED` state or wake-list: the
  calling task stays `TASK_READY` in the ordinary ready queue the
  whole time, just like `reaper_task`'s own pre-existing idle-`hlt`
  pattern.
- Found and fixed a genuine latent bug this change would otherwise have
  exposed: `syscall_entry.asm`'s `saved_user_rsp` was a single bare
  global, safe only because syscalls used to be atomic w.r.t.
  scheduling. `kernel/sched/task.h` gained a per-task
  `saved_user_rsp` field; `kernel/arch/x86_64/syscall.c` gained a
  scheduler-maintained indirection pointer
  (`syscall_set_user_rsp_slot()`, called from `scheduler.c`'s
  `timer_tick_handler` on every switch, mirroring the existing
  `syscall_kernel_rsp`/`TSS.RSP0` per-task pattern) so a blocked task's
  own user-mode RSP survives arbitrarily many OTHER tasks' syscalls
  happening while it waits.
- `kernel/user/fork_demo.asm`: simplified to a single blocking
  `sys_wait` call (old poll-and-spin wrapper deleted); child branch
  gained a bounded 200,000-iteration `sys_nop` spin (same magnitude
  `hello.asm`'s own `LOOP_COUNT` already uses) specifically to make the
  new self-test's outcome deterministic rather than a timing race.
- `kernel/kernel.c`: new self-test asserting
  `syscall_get_wait_block_count() > 0` — panics if sys_wait never
  actually took its blocking path this boot, since "returned the right
  answer" alone doesn't distinguish a genuine block from a lucky
  immediate success.
**Verification:** `make run` boots and prints every Milestone 1-19
marker unchanged plus `[OK] blocking wait self-test passed, sys_wait
genuinely blocked (0xN turns)...`, N >= 1 confirmed empirically across
5 repeat boots (N itself legitimately varies with host/QEMU timing —
observed 1 or 2 — but was never 0, by construction, see ADR 0020).
`-d int,cpu_reset` trace across a full boot: zero `#PF`/double-fault/
reset events, only expected IRQ traffic and the pre-existing deliberate
`#BP`. `tests/qemu/test_blocking_wait_selftest.sh` (new) independently
checks the new marker, its nonzero turn count, and its ordering after
the fork/wait exit-code check. All nineteen earlier smoke tests and all
four host test suites re-verified passing with no code changes needed
beyond one header-comment update
(`test_fork_wait_selftest.sh`, "non-blocking" → current behavior; its
actual assertions were already implementation-agnostic).
**Design record:** `docs/adr/0020-blocking-wait.md`.
**Known limitation (accepted for this milestone only):** a caller with
no children at all blocks forever (same underlying "no live child
list" tracking gap ADR 0018 already accepted, now surfacing as an
infinite block instead of an infinite poll). `sys_wait` is still the
ONLY blocking syscall — no general sleep-queue/wake primitive exists
yet; a future IPC/synchronization milestone would need one.

## 21. Copy-on-write fork — DONE
Motivated by Milestone 18's own named limitation (ADR 0018): `sys_fork`
was a full eager deep copy specifically because no COW mechanism
existed yet — wasteful for any page a child never actually modifies.
Now buildable on top of two things this session already shipped:
Milestone 19's general physical direct-map (a lazy copy's own
bookkeeping needs to read/write an arbitrary physical frame directly)
and, incidentally, Milestone 20's blocking `sys_wait` (reused as the
new isolation self-test's own synchronization primitive, no new IPC
needed).
**Proves:** forking a process now shares pages lazily — a write from
EITHER the parent or the child triggers a real, resolved `#PF` that
privatizes just that one page — verified as ACTUALLY exercised (not
just correct by luck) via a new fault-count self-test, AND verified as
genuinely isolated (not accidentally aliased) via a dedicated parent/
child divergent-write test.
**Deliverables:**
- `kernel/mm/pmm.c/.h`: a per-frame refcount array
  (`pmm_frame_addref()`/`pmm_frame_refcount()`, new;
  `pmm_free_frame()` now decrements-then-frees-at-zero instead of
  freeing unconditionally) — every pre-existing exclusive-ownership
  call site is behaviorally unchanged (refcount always goes 1 -> 0,
  same as before).
- `kernel/mm/vmm.h/.c`: new `VMM_FLAG_COW` PTE bit (bit 10);
  `vmm_fork_cow_page()` (downgrades the PARENT's own live mapping to
  read-only+COW in place via a new `find_pte()` walker, shares the
  same frame into the child, addrefs it — or, for an already-read-only
  page, just shares it outright, no COW needed); `vmm_handle_cow_fault()`
  (resolves a write fault: takes the frame over in place if this is
  already the last reference, otherwise copies via `vmm_phys_to_virt()`
  and drops the old reference) plus `vmm_get_cow_fault_count()`.
- `kernel/arch/x86_64/exceptions.c`: `isr_handler` checks for a COW
  write-fault (`#PF`, error code P=1 W=1) and calls
  `vmm_handle_cow_fault()` BEFORE any diagnostic printing — a resolved
  COW fault is silent, expected, successful control flow, not a panic.
  Confirmed (from `idt.c`, not assumed) that every exception vector is
  an interrupt gate, so this always runs with interrupts masked —
  never races another task's fault/fork on the same frame's refcount.
- `kernel/sched/task.c`: `task_fork()`'s page-copy visitor replaced
  with a one-line call to `vmm_fork_cow_page()` per page — the eager
  byte-copy loop is gone.
- `kernel/user/fork_demo.asm`: parent and child now both write a
  DIFFERENT sentinel to the same originally-shared `.data` variable;
  the parent's readback (guaranteed strictly after the child's own
  write+exit, via Milestone 20's blocking `sys_wait`) must see its OWN
  value, proving real isolation, not aliasing.
- `kernel/kernel.c`: new self-test panics if
  `vmm_get_cow_fault_count()` is ever 0 after the fork demo runs —
  proves sharing was genuinely lazy, not just correct.
**Verification:** `make run` boots and prints every Milestone 1-20
marker unchanged plus the new COW isolation and fault-count markers
(fault count = 3, hand-verified: parent's `child_pid` write, parent's
`shared_var` write [copy branch, still shared], child's `shared_var`
write [in-place-takeover branch, already the last reference]).
`-d int,cpu_reset` trace: exactly 3 `#PF` events (the first milestone
where `#PF` is an EXPECTED, counted vector rather than zero-tolerance),
zero double-fault/reset. `tests/qemu/test_cow_fork_selftest.sh` (new)
independently checks both markers, the >= 3 fault-count assertion, and
the frame-leak self-test. All twenty earlier smoke tests and all four
host test suites re-verified passing (one header-comment update,
`test_fork_wait_selftest.sh`, no assertion changes). Booted 5 times
back to back, correct every time, no flakiness.
**Design record:** `docs/adr/0021-cow-fork.md`.
**Known limitation (accepted for this milestone only):** no demand
paging / lazy allocation beyond fork's own COW sharing — every other
page is still eagerly allocated. No VMA tracking (a separate future
item). `frame_refcount[]` is `uint16_t` — a single frame shared by more
than 65535 concurrent address spaces would silently wrap, accepted as
unreachable at this kernel's current scale.

## 22. `sys_exec` — DONE
Motivated by Milestone 18's own named limitation (ADR 0018) and
`future.md`'s "reasonable next steps": `sys_fork` gave a process the
ability to spawn a child, but no way to replace its OWN running image
with a different one — the other half of the fork/exec pair.
`future.md` had flagged this as needing "a genuinely NEW control-flow
primitive, a synchronous mid-syscall resume via `iretq`" — re-reading
`syscall_entry.asm` before writing any code found that assumption was
wrong (see Decision).
**Proves:** a ring-3 process can `sys_exec` into a completely different
embedded program and have that program's code genuinely start running
in its OWN, SAME process (same `task_t`, same pid, same PML4 frame) —
not a new process, and not a no-op that only appeared to work.
**Deliverables:**
- `kernel/mm/vmm.h/.c`: `vmm_reset_user_address_space()`, a new sibling
  of `vmm_destroy_address_space()` with the OPPOSITE activity
  requirement — MUST be called on the CURRENTLY ACTIVE address space
  (destroy's contract requires the target NOT be active). Both now share
  one factored-out walk (`free_process_private_frames()`, a pure
  refactor of destroy's existing loop) that additionally `invlpg`s each
  leaf's own reconstructed VA as it's cleared — a no-op for destroy
  (target already inactive), load-bearing for reset (nothing stale is
  left in the TLB for `elf_load()`'s subsequent mappings to be shadowed
  by).
- `kernel/sched/task.h/.c`: `task_exec()` — validates the requested
  `program_id` against a small fixed table BEFORE tearing anything down
  (a bad id leaves the process completely untouched), then
  `vmm_reset_user_address_space()`s the caller's own address space,
  `elf_load()`s the new image into it, maps a fresh user stack, and
  overwrites the CURRENT syscall's own `syscall_frame_t` in place
  (`rcx` = new `e_entry`, `r11` = fresh RFLAGS, every other GPR zeroed)
  plus `task->saved_user_rsp` — no assembly changed; `syscall_entry.asm`'s
  existing `sysretq` epilogue just resumes into whatever `rcx`/`r11`/the
  per-task RSP now say, which happens to be a different program this
  time.
- `kernel/arch/x86_64/syscall.h/.c`: `SYS_EXEC` (5); `sys_exec()` (thin
  wrapper — reports -1 on a bad `program_id`, otherwise counts the
  success via `syscall_get_exec_count()`).
- `kernel/user/exec_demo.asm`, `kernel/user/exec_target.asm` (new, both
  embedded via the established `incbin` blob pattern): two genuinely
  DIFFERENT programs — not a reuse of `hello.elf`/`fork_demo.elf`, whose
  own self-tests count their own messages an exact number of times.
- `kernel/kernel.c`: a fifth orphan process (`exec_demo_process`); new
  self-test panics if `syscall_get_exec_count()` is ever 0; the process-
  lifecycle reap-count threshold raised from 4 to 5 (exec reuses its
  caller's own task, not a sixth new one).
**Verification:** `make run` boots and prints every Milestone 1-21
marker unchanged plus `[OK] exec demo process created...` → `[OK] exec
demo running, about to sys_exec into a new image` → `[OK] exec target
running -- process image was genuinely replaced by sys_exec` → (later)
exactly one `[OK] process 0x7 exited and was reaped` line (same pid,
proving reuse, not creation) → `[OK] exec self-test passed...`.
`-d int,cpu_reset` trace: still exactly 3 `#PF` events (unchanged from
Milestone 21 — `sys_exec`'s reset-and-repopulate cycle never faults),
zero double-fault/reset. `tests/qemu/test_exec_selftest.sh` (new)
independently checks real sequencing, absence of the demo's own
`[FAIL]` marker, the exact reap count (5, not 6), and the frame-leak
self-test. `test_fork_wait_selftest.sh`/`test_process_lifecycle_selftest.sh`
needed their exact reaped-count assertion updated (4 → 4→5, scope
growth, not a behavior fix). All twenty-one earlier smoke tests and all
four host test suites re-verified passing. Correct on the first real
boot attempt; booted 5 times back to back, exec-specific sequence and
every count identical every time.
**Design record:** `docs/adr/0022-sys-exec.md` — including the full
reasoning for why the originally-feared new control-flow primitive
turned out to be unnecessary.
**Known limitation (accepted for this milestone only):** can only
target a small, fixed, build-time-embedded set of images (currently
one) — no filesystem exists for a real path-based `execve`. No
argv/envp-equivalent is passed across the swap (every GPR is zeroed,
matching this kernel's existing "programs take no arguments"
convention). A bad `program_id` is the only validated-input failure
path; an ELF64 validation failure on the embedded target image still
panics (build-time-trusted, same stance `task_create_user_image()`
already takes).

## 23. Graphics framebuffer console and mouse cursor — DONE
Motivated by Milestone 16's own named limitation (ADR 0016 — the mouse
driver had nothing to draw a cursor on) and `future.md`'s "reasonable
next steps." A real design fork was found before writing any code and
checked with the user rather than guessed past: satisfying a Multiboot2
framebuffer request switches the video HARDWARE mode, so VGA text mode
(Milestone 8) and a linear graphics framebuffer can't run
simultaneously — drawing a cursor on pixels would otherwise silently
regress the existing on-screen shell. The user chose full replacement:
real text rendering on the framebuffer (closing ADR 0008's own flagged
UEFI-without-CSM gap too), not a narrower opt-in demo.
**Proves:** the kernel negotiates a real linear framebuffer via
Multiboot2, renders its ENTIRE console (boot log + interactive shell) as
correctly-formed, legible pixel text, and moves a mouse cursor to the
EXACT expected pixel position in response to genuine synthetic PS/2
input — not just that a framebuffer driver exists.
**Deliverables:**
- `kernel/arch/x86_64/boot.asm`: Multiboot2 framebuffer request tag
  (1024x768x32, optional), verified against the canonical GRUB header
  before writing any code (same primary source as ADR 0001). Confirmed
  via a temporary probe that GRUB honors it with ZERO `grub.cfg` changes
  needed — tested incrementally before building anything on top, per
  CLAUDE.md's boot-protocol discipline.
- `kernel/arch/x86_64/multiboot2.h/.c`: framebuffer boot-info tag
  struct; a new shared `multiboot2_find_tag()` helper, factored out once
  `framebuffer.c` became a second real consumer of `pmm.c`'s existing
  inline tag-walk (which was refactored to use it too — pure,
  behavior-preserving).
- `kernel/drivers/framebuffer.h/.c` (new): `fb_init()` reads the
  negotiated mode back from the boot-info tag (never hardcodes the
  request) and reaches its physical memory via the EXISTING Milestone 19
  direct-map (`vmm_phys_to_virt()`) — QEMU's framebuffer BAR
  (`0xfd000000`, observed) sits well under 4GiB, so ZERO new page-table
  work was needed. `fb_put_pixel`/`fb_fill_rect`/`fb_read_rect`/
  `fb_scroll_up`/`fb_pack_color`, all guarded by an `fb_ready` flag so a
  panic/fault dump that fires before `fb_init()` has run (unavoidably
  later in boot than the old `vga_init()` — needs the direct-map first)
  can never itself crash.
- `kernel/drivers/font8x8.h` (new): a real, well-known public-domain 8x8
  bitmap font, fetched and byte-for-byte extracted from a real source
  (`dhepper/font8x8`) rather than hand-authored from memory — a
  first-try extraction regex bug (a glyph's own trailing comment
  containing a literal `{` confused a naive parser) was caught by
  validating every row's byte count before embedding, not by a garbled
  on-screen glyph.
- `kernel/drivers/fbconsole.h/.c` (new): the text console, same
  `putc`/`clear`/`write` interface `vga.c` had (one-line swap in
  `console.c`'s fan-out), cell grid sized from the ACTUAL negotiated
  resolution (128x96 cells), scrolling via `fb_scroll_up()`'s raw
  pixel-row copy.
- `kernel/drivers/cursor.h/.c` (new): a save/restore sprite pattern
  (`fb_read_rect`/`fb_fill_rect`) moved by real mouse deltas; erase (old
  position) happens strictly before the position updates and the new
  save+draw, an ordering worked out on paper before writing code.
- `kernel/drivers/mouse.h/.c`: a SECOND, independent event queue
  (`mouse_has_cursor_event()`/`mouse_get_cursor_event()`) — found and
  designed around BEFORE it broke anything: `cursor_poll()` and the
  shell's existing `mouse` command both need every decoded packet, and
  a single shared queue would let one silently steal the other's event,
  which would have broken `test_mouse_selftest.sh`'s already-passing
  contract.
- `kernel/shell.c`: `read_line()`'s existing keyboard-wait loop also
  calls `cursor_poll()` every `hlt` wakeup — no new polling loop or
  scheduling primitive needed.
- `kernel/drivers/vga.c`/`vga.h`: retired (deleted) — once graphics mode
  is active, `0xB8000` is no longer live text-mode memory; keeping it
  around unused would be actively misleading, not just dead code.
**Verification:** `make run` boots and prints every Milestone 1-22
marker unchanged (through serial only, until the direct-map exists),
then `[OK] graphics framebuffer console initialized`, after which every
message reaches both serial and the screen. A QEMU `screendump` at the
shell prompt, visually inspected, shows every boot-log line correctly
rendered — the font's bit-order convention was right on the first
attempt, confirmed by looking, not trusted from memory. `-d
int,cpu_reset` trace: unchanged from Milestone 22 (1 `#BP`, 3 `#PF`,
zero double-fault/reset) — this milestone added no new fault-driven code
paths. `tests/qemu/test_framebuffer_selftest.sh` (new) takes two real
screendumps around a real injected `mouse_move 100 50` and asserts the
cursor's 8x8 block is at the EXACT expected pixel position both before
and after, with no leftover pixels at the old position. All twenty-two
earlier smoke tests (`test_mouse_selftest.sh`/`test_shell_selftest.sh`
specifically re-verified, both directly touched by this milestone) and
all four host test suites re-verified passing with no assertion
changes needed. Correct on the first real boot attempt; booted 5 times
back to back, identical shape every time.
**Design record:** `docs/adr/0023-framebuffer-console-and-cursor.md`.
**Known limitation (accepted for this milestone only):** no blinking
text-input caret; the mouse cursor is a plain filled square, not a real
arrow shape; no MTRR/PAT tuning of the framebuffer's memory-type
attributes; only 32bpp direct-color RGB is supported (panics on
INDEXED/EGA_TEXT, neither ever observed from QEMU's default machine);
the visible graphics boot log starts partway through boot (after the
direct-map is ready), not at `kernel_main`'s very first line — every
message still reaches serial throughout, so nothing is silently lost.

## 24. Minimal userspace C runtime — DONE
First step of the GUI arc scoped in `Desktop.md` (multi-window desktop,
filesystem staying a non-goal, confirmed with the user). Every user
program through Milestone 23 was hand-written NASM — tractable for four
small linear demos, not for what's coming (a window server, several
real apps).
**Proves:** a ring-3 program compiled from ordinary C (not hand-written
assembly) runs correctly under this kernel's existing ELF loader/
process model — proven the strongest available way: `kernel/user/
hello.asm` (Milestone 17) was REWRITTEN in C using the new runtime and
produces byte-for-byte the same behavior, so every existing test
asserting on it needed zero changes.
**Deliverables:**
- `kernel/user/rt/crt0.asm` (new): entry stub bridging the raw
  `iretq`/`sysretq` entry context (RSP 16-aligned, no return address
  pushed) into one `main()` can safely run in as an ordinary SysV
  function — the same `and rsp, ~0xf` fix `boot.asm`'s own
  `higher_half_entry` already applies before its first C call.
- `kernel/user/rt/syscall.h/.c` (new): thin C wrappers around this
  kernel's own six syscalls — explicitly NOT the "POSIX userland"
  non-goal, flagged as such in `Desktop.md` before this milestone
  started.
- `kernel/user/rt/string.h/.c` (new): minimal `memset`/`memcpy`/
  `memmove`/`strlen`, both for explicit use and to satisfy any
  implicit compiler-generated calls.
- `Makefile`: `USER_CFLAGS` (`-mcmodel=large`, no `-mno-red-zone`,
  otherwise matching the kernel's freestanding/no-FP stance) — a
  separate flag set from the kernel's own `CFLAGS`, applied via a
  static pattern rule scoped to exactly the userspace C sources.
- `kernel/user/hello.asm` retired (deleted), replaced in place by
  `kernel/user/hello.c` — same two messages, same `.data`/`.bss`
  correctness check, same bounded `sys_nop` spin every existing
  self-test already counted on.
**Verification:** `make run` boots and prints every Milestone 1-23
marker unchanged, including both `hello.elf` instances' exact messages
(each exactly twice) — now produced by compiled C, confirmed via the
build log showing a real `x86_64-elf-gcc` invocation on `hello.c`, not
an accidental no-op. `readelf`/`nm` on the linked `hello.elf` confirmed
`_start` at the expected `0x8000400000` with no truncated-relocation
link errors before ever booting it. `-d int,cpu_reset` trace: unchanged
from Milestone 23 (1 `#BP`, 3 `#PF`, zero double-fault/reset). All
twenty-three earlier smoke tests and all four host test suites
re-verified passing with ZERO assertion changes — the strongest
possible regression signal for a runtime swap underneath an
already-tested program. Correct on the first real boot attempt; booted
4 times back to back, identical shape every time.
**Design record:** `docs/adr/0024-userspace-c-runtime.md`.
**Known limitation (accepted for this milestone only):** only two
syscall arguments' worth of capacity (sufficient for every syscall this
kernel has today); no `argv`/`envp`/heap allocator in the runtime yet —
added only once a later GUI-arc milestone actually needs one.

## 25. General blocking/wake scheduler primitive — DONE
Second step of the GUI arc (`Desktop.md`). `future.md` had flagged this
as deferred pending "a second real reason for two processes to
synchronize" — the arc's own event-loop/IPC needs are that reason.
Generalizes Milestone 20's one-off `sys_wait`-specific `sti;hlt;cli`
retry loop (where the caller stayed `TASK_READY` the whole time,
wastefully polling) into a real `TASK_BLOCKED` state.
**Proves:** a task can genuinely block (leave the ready queue entirely,
consuming zero further scheduler turns) and later resume only once
another task explicitly wakes it — verified deterministically, not via
a timing race.
**Deliverables:**
- `kernel/sched/task.h`: new `TASK_BLOCKED` state.
- `kernel/sched/scheduler.h/.c`: `scheduler_block_current()` (marks the
  caller blocked, `sti`, `hlt`-loops until woken, `cli` before
  returning — same in/out IF invariant `sys_wait`'s own loop already
  had) and `scheduler_wake(task_t *task)` (a no-op unless the target is
  actually blocked, otherwise relinks it via the existing
  `scheduler_add_task()`). `timer_tick_handler` gained an `else if
  (outgoing->state == TASK_BLOCKED)` branch unlinking it from the ready
  queue the same way `TASK_ZOMBIE` already is — no separate list needed
  (unlike `zombie_head`), since a waker always already holds the
  specific `task_t*` to wake.
- `kernel/kernel.c`: two new kernel threads with a dedicated,
  deterministic-by-construction self-test (an explicit go/no-go handoff
  flag, not a tuned delay — a strictly more robust technique than
  Milestone 20's own, made possible here because this test controls
  both sides of the interaction).
**Verification:** `make run` boots and prints every Milestone 1-24
marker unchanged plus the new self-test's two markers in the correct
order. **Two real bugs were found and fixed** during this milestone —
the first live-boot bug since Milestone 16: (1) a raw `pushfq` held
open across intervening C code in the first draft of `scheduler_wake()`
(GCC doesn't track a bare `RSP` shift it didn't emit itself), caught in
review before ever booting, fixed via `pushfq; pop %0` inside one atomic
asm block instead; (2) the actual boot hang this exposed --
`scheduler_block_current()`'s "always returns with `IF=0`" contract is
correct for `sys_wait`'s syscall-context caller but wrong for a KERNEL
THREAD caller with its own trailing `hlt` loop and nothing else to
re-enable interrupts, freezing the entire single-CPU machine (`hlt`
with `IF=0` waits for an NMI that never comes) -- diagnosed from the
tell that EVERY other unrelated task's progress also stopped dead at
the same point, not just this test; fixed by adding the same explicit
`sti` `scheduler_exit_current()` already uses before its own trailing
loop, for the identical reason. `-d int,cpu_reset` trace (after both
fixes): unchanged from Milestone 24. `tests/qemu/
test_scheduler_wake_selftest.sh` (new) checks both markers, correct
ordering, and the frame-leak self-test. All twenty-three earlier smoke
tests and all four host test suites re-verified passing. Booted 5 times
back to back after the fixes — identical shape every time.
**Design record:** `docs/adr/0025-blocking-wake-scheduler-primitive.md`
— including the full diagnostic trail for both bugs.
**Known limitation (accepted for this milestone only):** `sys_wait`
still uses its own Milestone 20 polling loop, not rewired onto this
primitive (a deliberate scope boundary, not an oversight — no concrete
benefit this milestone needs); no timeout support; no
priority/fairness policy for multiple tasks blocked on the same
resource (no current caller has more than one yet).

## 26. IPC message passing and shared memory — DONE
Third step of the GUI arc (`Desktop.md`), following the userspace C
runtime (Milestone 24) and the general blocking/wake primitive
(Milestone 25). A display server and its client apps are genuinely
separate, isolated processes (Milestone 9) — they need a way to
exchange control messages and bulk pixel data without copying through
a filesystem that doesn't exist.
**Proves:** two genuinely isolated processes exchange a real message
and a real block of shared physical memory — a known pattern written
by one side is read back correctly by the other through a completely
independent virtual address — with `sys_ipc_recv`'s blocking path
verified as genuinely exercised, deterministically.
**Deliverables:**
- `kernel/sched/scheduler.h/.c`: a general `pid -> task_t*` lookup
  (`scheduler_register_task()`/`scheduler_unregister_task()`/
  `scheduler_find_task()`, a small fixed-capacity registry) — needed
  because `sys_ipc_send` addresses its destination by pid, and unlike
  every prior `task_t*` consumer, the destination could be
  `TASK_BLOCKED` (unlinked from the ready queue, not searchable there
  per Milestone 25's own design).
- `kernel/ipc/ipc_message.h` (new): one shared wire-format struct,
  included unmodified by both kernel and userspace runtime code.
- `kernel/ipc/msgqueue.h/.c` (new): `ipc_send()`/`ipc_try_recv()`, a
  small fixed-capacity inbox embedded directly in `task_t`.
  `sys_ipc_recv` (`syscall.c`) layers blocking on top via
  `scheduler_block_current()` — its first REAL consumer outside
  Milestone 25's own self-test.
- `kernel/ipc/shm.h/.c` (new): named shared-memory objects — small,
  fixed-capacity, deliberately narrower than general VMA tracking
  (`future.md`'s long-deferred item, whose actual trigger turned out
  narrower than expected). Cleanup reuses `pmm.h`'s EXISTING
  refcounting (Milestone 21's COW mechanism) via each mapper's own
  `VMM_FLAG_OWNED` mapping — no new "destroy" API needed.
- `kernel/sched/task.h/.c`: a per-task inbox and a per-task shm VA bump
  allocator (`shm_next_va`) — independent per process, not a shared
  global counter.
- `kernel/arch/x86_64/syscall.h/.c`: `SYS_IPC_SEND`/`SYS_IPC_RECV`/
  `SYS_SHM_CREATE`/`SYS_SHM_MAP`.
- `kernel/user/rt/syscall.h/.c`: userspace wrappers.
- `kernel/user/ipc_sender.c`/`ipc_receiver.c` (new): the actual demo —
  `kernel_main` acts as a trusted "init," injecting a bootstrap message
  (a kernel-side `ipc_send()` call, not a syscall) so the sender learns
  the receiver's pid with no `argv`/`envp` yet.
**Verification:** `make run` boots and prints every Milestone 1-25
marker unchanged plus the new demo's markers in sequence, ending with
`[OK] ipc self-test passed, sys_ipc_recv genuinely blocked (0x1
turns)...`. **Three real bugs were found and fixed**, each from an
actual observed symptom: (1) `ipc_sender.c` checked the wrong success/
failure convention for `sys_ipc_send` (a genuine contradiction in the
log — the sender reported failure yet the receiver still verified the
correct pattern — pointed straight at the real bug); (2) a real frame
leak, caught by the process-lifecycle self-test's exact-baseline check
— `shm_map()`'s first draft double-counted the first mapper's own
reference against `pmm_alloc_frame()`'s own implicit one; (3)
`sys_ipc_recv_block_count` stayed 0 because `scheduler_add_task()`
inserts each new task right after `current_task` (unchanged throughout
`kernel_main`'s interrupts-off setup), so the LAST task added runs
FIRST once preemption begins — the original receiver-then-sender
creation order actually scheduled the sender first, letting it deliver
before the receiver's first check. A fourth bug (`task_fork()`
resetting a child's shm VA bump pointer, which would have collided with
COW-inherited shm mappings) was caught in review before ever booting.
`-d int,cpu_reset` trace (after all fixes): unchanged from Milestone
25. `tests/qemu/test_ipc_shm_selftest.sh` (new) checks every marker,
real sequencing, the exact reap count (7, up from 5 —
`test_exec_selftest.sh`/`test_fork_wait_selftest.sh`/
`test_process_lifecycle_selftest.sh` all needed this same assertion
updated), and the frame-leak self-test. All twenty-four earlier smoke
tests and all four host test suites re-verified passing. Booted 5 times
back to back after the fixes — identical shape (including the exact
block count) every time.
**Design record:**
`docs/adr/0026-ipc-message-passing-and-shared-memory.md` — including
the full diagnostic trail for all four bugs.
**Known limitation (accepted for this milestone only):** fixed-size,
fixed-field-count messages only, no variable-length payload;
`ipc_send()` drops silently on a full inbox (matching `mouse.c`'s own
lossy-by-design event queues); shared-memory objects capped at 1MiB/
process and 16 objects total; no priority/fairness policy for multiple
tasks blocked on the same resource; `sys_ipc_recv` always blocks, no
non-blocking variant.

## 27. Minimal display server, one client, no overlap

Fourth step of the GUI arc (`Desktop.md`) and its own flagged "actual
hard-unknown milestone": prove the client-server display model works
at all before any multi-window logic (z-order, damage tracking, input
focus) goes on top of it. One server process
(`kernel/user/display_server.c`) claims sole ownership of the real
graphics framebuffer; one client (`kernel/user/display_client.c`) asks
for a canvas and gets back one no larger than the server's own fixed
maximum, regardless of what it asked for.

**Deliverables:**
- `kernel/arch/x86_64/syscall.h/.c`: two new syscalls.
  `SYS_FB_ACQUIRE` claims sole framebuffer ownership — succeeds exactly
  once, ever, for the whole boot (kernel-enforced by pid, not a
  userspace convention); returns packed `(width << 32) | height`.
  `SYS_FB_PRESENT(x, y, w, h, buf_va)` blits a validated caller-owned
  buffer (plain `0x00RRGGBB` pixels, decoupled from this framebuffer's
  own negotiated bit layout via the EXISTING `fb_pack_color()`) onto
  the real screen — only the current owner may ever call it.
- `kernel/user/rt/syscall.h/.c`: userspace wrappers, including this
  runtime's first 5-argument syscall (`syscall5()`, GCC register
  variables for r10/r8 — the standard idiom, same one Linux's own raw
  `syscall()` wrapper uses).
- `kernel/user/display_protocol.h` (new): a tiny 3-message protocol
  (REQUEST/GRANT/PRESENT) layered entirely on the EXISTING
  `ipc_message_t`/`sys_ipc_send`/`sys_ipc_recv` mechanism (Milestone
  26) — no new IPC machinery. A client's canvas is an ordinary
  Milestone-26 shared-memory object.
- `kernel/user/display_server.c`/`display_client.c` (new): the actual
  demo. The client deliberately asks for 400x300; the server's own
  fixed policy (`MAX_CANVAS_W`/`H = 200x150`) never grants more than
  that — this IS "the server enforces the bound," implemented in
  userspace policy, not a kernel clamp. The client never even
  allocates a buffer bigger than what it was granted, so the
  bound-enforcement proof is structural, not just a runtime check.
**Verification:** `make run` boots and prints every Milestone 1-26
marker unchanged plus the new demo's markers in sequence.
**The real proof is pixel-level, not just a self-report:**
`tests/qemu/test_display_server_selftest.sh` (new) takes a real QEMU
monitor `screendump` (Milestone 23's own technique) after the shell
prompt appears, scans the whole framebuffer for the client's
distinctive fill color, and asserts the resulting bounding box is
EXACTLY the server's granted 200x150 canvas at (100,100) — not the
400x300 the client asked for. Ownership exclusivity is proven both
within one process (the server's own second `sys_fb_acquire()` fails)
and across processes (the client's own attempt fails too, causally
after the server's successful one via the protocol's own blocking
IPC — no scheduling-order assumption needed, unlike Milestone 26's own
process-creation-order bug). `-d int,cpu_reset` trace unchanged from
Milestone 26. All twenty-five earlier smoke tests plus the new one, and
all four host test suites, pass. Reap count raised 7 → 9
(`test_exec_selftest.sh`/`test_fork_wait_selftest.sh`/
`test_process_lifecycle_selftest.sh`/`test_ipc_shm_selftest.sh` all
needed this same assertion updated). Booted clean on the FIRST real
attempt, then 3 additional repeat boots, identical shape every time —
no live bugs this milestone, every design question worked through in
review before ever running QEMU.
**Design record:** `docs/adr/0027-minimal-display-server.md`.
**Known limitations (accepted for this milestone only):** exactly one
server, one client — no window list/z-order/damage tracking/input
routing yet; canvas position is a fixed constant, not negotiated or
movable; framebuffer ownership is never released even after the owning
process exits; no per-owner kernel-side canvas cap (this milestone's
own userspace policy is the only enforcement — fine for a cooperating
demo client, not a substitute for kernel enforcement against a
genuinely untrusted one); no damage/dirty-rectangle tracking, every
present re-blits the whole rectangle.

## 28. Multiple windows and z-order compositing

Fifth step of the GUI arc (`Desktop.md`). `Desktop.md`'s own milestone 5
bundled "z-order, damage tracking, input focus" as one item, written
speculatively before any of it was built; this milestone deliberately
covers only z-order compositing — real click-driven input focus needs a
genuinely separate new subsystem (routing a hardware input event to a
ring-3 process, which nothing in this kernel does yet) and was split
into its own later milestone instead (see ADR 0028's Decision).

**Deliverables:**
- `kernel/user/display_server.c` extended from Milestone 27's single
  client to two (`WINDOWS_TOTAL = 2`), cascaded (+50, +50) so their
  granted 200x150 canvases genuinely overlap (a real 150x100 shared
  region) — served in a flat, strictly sequential loop, correct only
  because the clients themselves guarantee strict arrival order (see
  next point), not a general per-pid state machine.
- `kernel/user/display_client_a.c`/`display_client_b.c` (renamed/added
  from Milestone 27's single client): client A sends client B an
  explicit `DISPLAY_OP_GO` hand-off, but only AFTER receiving a NEW
  `DISPLAY_OP_ACK` from the server confirming its own canvas is
  genuinely already composited (`sys_ipc_send()` only proves a message
  was enqueued, never that the destination acted on it — a real gap
  caught in review before ever booting) — making the resulting z-order
  deterministic by construction, not a race.
- Since both windows are fully opaque, correct z-order needs no
  compositing/damage-tracking pass at all — just presentation order;
  a later window's pixels naturally win any overlap. No new kernel
  syscalls were needed at all for this milestone.
**Verification:** `make run` boots and prints every Milestone 1-27
marker unchanged plus both windows' markers in the correct sequence,
ending with `sys_fb_present blitted 0x2 frame(s)` (`kernel_main`'s own
self-test now checks for EXACTLY 2, not just ">0").
**The real proof is pixel-level:** `tests/qemu/test_display_server_selftest.sh`
(rewritten for this milestone) takes a real QEMU screendump and
confirms client B's canvas is a full, unbroken rectangle while client
A's is reduced to an L-shape with the same outer extent — plus
spot-checks proving the exact occlusion direction, not just that both
colors exist somewhere.
**Two real bugs found along the way, not planned in advance:** (1) the
smoke test's own first draft, using hardcoded absolute screen
coordinates, failed — root-caused to `kernel/drivers/fbconsole.c`'s
`fb_scroll_up()` shifting the entire framebuffer (including
already-drawn windows) once this milestone's extra boot-time console
output pushed a scroll threshold no earlier milestone had reached;
fixed by checking geometry relative to the windows' own discovered
position rather than an absolute constant. (2) The SAME extra scroll
volume exposed a genuine, real ghost-trail regression in
`kernel/drivers/cursor.c` (Milestone 23) — a scroll it had no way to
know about left its own save/restore bookkeeping stale, caught by that
milestone's own pre-existing `test_framebuffer_selftest.sh` check;
fixed with two new public functions, `cursor_hide()`/`cursor_show()`,
wrapped around `fbconsole.c`'s own scroll call (a small, deliberate new
`fbconsole.c` → `cursor.c` coupling). `-d int,cpu_reset` trace unchanged
from Milestone 27. All twenty-five other smoke tests and all four host
test suites pass. Reap count raised 9 → 10.
**Design record:** `docs/adr/0028-multiple-windows-z-order.md`.
**Known limitations (accepted for this milestone only):** exactly two
clients, fixed cascade placement, no dynamic window list/move/close/
raise, no real input-driven focus yet. Windows themselves (unlike the
now-fixed cursor) are NOT immune to later console scroll — a real,
growing architectural gap (the text console and the window/cursor
compositing layer share one physical framebuffer with no separate
surface, and no "please redraw yourself" protocol for a ring-3 window
owner) worth a dedicated fix before window chrome/interactivity makes
a visibly drifting window user-facing, not just a test inconvenience.

## 29. Real input-driven click routing (hardware event → userspace IPC)

Sixth step of the GUI arc (`Desktop.md`) — the genuinely-new-subsystem
half of the milestone 5 arc item Milestone 28 deliberately split in
two: delivering a real hardware input event to a ring-3 process at
all, something nothing in this kernel had ever done before (every
prior mouse-event consumer was kernel-side code).

**Deliverables:**
- `kernel/arch/x86_64/syscall.h/.c`: `SYS_INPUT_SUBSCRIBE` — registers
  the calling process as the sole recipient of routed input events.
  Kernel-enforced exclusivity, same pattern as `SYS_FB_ACQUIRE`
  (Milestone 27) but a deliberately SEPARATE global (`input_focus_pid`,
  not `fb_owner_pid`) — "who owns the framebuffer" and "who receives
  input" are different questions, even though this milestone's own
  demo happens to answer both the same way.
- `kernel/drivers/input_router.c`/`.h` (new): `input_router_notify_click()`
  looks up the current subscriber and delivers an `INPUT_EVENT_CLICK`
  (`kernel/user/input_protocol.h`, new) via the EXISTING
  `ipc_send()` mechanism (Milestone 26) — no new IPC machinery.
- `kernel/drivers/cursor.c` extended to detect a genuine left-click
  EDGE (the PS/2 wire format only ever reports a level, not a
  transition) and call the router at the cursor's own, already-tracked
  on-screen position — the natural place, since it already sees every
  event's button state.
- `kernel/user/input_focus_demo.c` (new): subscribes, proves
  exclusivity (a second subscribe attempt fails, mirroring
  `sys_fb_acquire()`'s own self-check), then blocks for exactly one
  real click and exits.
**A real structural conflict found and fixed in review, before ever
booting:** a process that blocks forever for external input cannot be
part of `kernel_main`'s own deterministic reap-count self-test gate —
every other headless test (no monitor injection) would hang forever
waiting for a reap that can't happen. Fixed by creating the demo
BEFORE the frame-leak baseline snapshot, alongside the permanent kernel
threads rather than the bounded demo processes, so neither the
reap-count gate nor the leak check ever depends on it exiting during a
normal boot.
**Verification:** `make run` boots and prints every Milestone 1-28
marker unchanged, plus the demo's own subscribe/exclusivity markers,
with the demo then sitting harmlessly blocked for the rest of a normal
boot (confirmed: every pre-existing test still shows exactly 10 reaps,
unchanged). `tests/qemu/test_input_focus_selftest.sh` (new) injects a
REAL `mouse_move`/`mouse_button` through the QEMU monitor (Milestones
16/23's own technique) and confirms the routed click lands at the
EXACT expected screen position (read from the framebuffer's own
self-reported negotiated dimensions, not hardcoded), the demo receives
it, and it then exits (11 reaps — the baseline 10 plus this one).
`-d int,cpu_reset` trace unchanged from Milestone 28. All twenty-six
other smoke tests and all four host suites pass. Deliberately NO
`kernel_main` panic-on-zero self-test for click delivery (unlike every
other "prove the path was exercised" counter this project has added) —
nothing in this kernel can synthesize a real click from inside
`kernel_main` itself, so such a check would break every headless test
that doesn't happen to inject one.
**Design record:** `docs/adr/0029-real-input-driven-click-routing.md`.
**Known limitations (accepted for this milestone only):** exactly one
subscriber, ever, for the whole boot, never released; only left-click
DOWN edges, no release/drag/right/middle button. Not yet wired to
anything that acts on it visually — no window raises/focuses in
response yet, that's `Desktop.md`'s own next arc item, building
directly on this delivery path.

## 30. Real click-driven window raising, and scroll-immune windows

Wires Milestone 29's click-delivery mechanism into an actual visible
effect for the first time: `kernel/user/display_server.c` redesigned
from "serve N clients then exit" (Milestones 27/28) into a genuinely
persistent process that raises a window in response to a real click.

**Deliverables:**
- `display_server.c` retired Milestone 29's standalone
  `input_focus_demo.c` (an unresolvable subscription-exclusivity
  conflict otherwise) and became the real input subscriber itself,
  moved to be created before `kernel_main`'s frame-leak baseline (never
  exits during a normal boot) — the first time the reap-count self-test
  gate has ever LOWERED (10 → 9), not raised.
- A real hit-test (topmost z-order position first) + a 2-element
  z-order swap on raise, recompositing via the exact same "opaque
  windows, painted bottom-to-top" reasoning Milestone 28 already
  established — no new compositing machinery.
- **The real fix for Milestone 28's own flagged, deferred gap** (ADR
  0028's Known limitations: windows drifting from console scroll) —
  which THIS milestone's own hit-testing turned into a genuine
  functional bug, not just cosmetic drift. `fb_scroll_up()`
  (`kernel/drivers/framebuffer.c`) gained a `region_height` parameter;
  `fbconsole.c` now reserves a fixed 480px region for console text;
  windows relocated to y ≥ 480, permanently outside the scroll's reach.
- **Two more real bugs found via direct evidence while verifying the
  above, neither planned in advance:** `draw_glyph()` corrupting the
  cursor sprite (no `cursor_hide()`/`cursor_show()` awareness at all);
  and the actual root cause of a real screendump-visible corruption —
  `console_putc()`'s shared cursor state has NO mutual exclusion
  against preemption, so two ring-3 processes' interleaved `sys_write()`
  calls could genuinely corrupt it once enough processes existed to
  make the race likely. Fixed with a short, save/restore-flags
  interrupt-disabled critical section (the same idiom
  `scheduler_wake()` established, Milestone 25), not a bare `cli`/`sti`.
**Verification:** `make run` boots and prints every Milestone 1-29
marker unchanged plus the server's own subscribe markers.
**The real proof is a second screendump:** `tests/qemu/test_display_server_selftest.sh`
(substantially rewritten) injects a real click onto client A's own
exclusive region, waits for the server's own "raised window" log line,
then confirms via a SECOND QEMU screendump that the overlap region
genuinely flips from client B's color to client A's — client A now the
full unbroken rectangle, client B reduced to the L-shape Milestone 28's
own technique already established for the opposite direction.
`-d int,cpu_reset` trace unchanged. All twenty-five other smoke tests
and all four host suites pass. Reap count lowered 10 → 9, with the
process-lifecycle self-test now checking an exact, DERIVED (not
hand-waved) 60-page deficit for the server's own two permanently-held
canvas references, verified against real captured frame counts before
being hardcoded.
**Design record:** `docs/adr/0030-window-raising-and-scroll-immune-windows.md`.
**Known limitations (accepted for this milestone only):** still exactly
two clients, fixed cascade placement, no dynamic window list/move/
close/chrome/widgets yet (`Desktop.md`'s own next arc item). No drag,
no close, no keyboard focus routing. The console's reserved height
(480px) is a fixed constant, not derived from actual boot text volume.

## 31. Window chrome and basic widgets (sequence TBD)

Milestone 31 is intentionally left as a one-line placeholder here — full
breakdown (deliverables/acceptance criteria/estimates/risks) gets written
up when that milestone actually starts, not in advance, to avoid designing
against assumptions already-implemented milestones might overturn. Next
in sequence per `Desktop.md`'s GUI arc: draggable/closable title bars
and at least one interactive widget, then real applications. See
`Desktop.md` for the full sequencing and `future.md` for the rest of
this project's continuation briefing. Separately, still awaiting the
user's decision: a disk driver + real filesystem, ACPI-based shutdown,
and SMP/networking, all explicitly flagged non-goals.
