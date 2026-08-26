# ADR 0032: real-hardware-only ring-3 SS corruption, and safe KVM acceleration

## Status
Accepted and verified — a real, reproducible `#GP General Protection`
fault, present on REAL HARDWARE (via KVM) since Milestone 18/21's
original fork/COW-fault code but invisible under TCG for 30+
milestones, has been found, root-caused with hard evidence (not
guessed), and fixed. `make run`/`make debug` now safely enable KVM
acceleration when available — which was the actual fix needed for this
session's original reported problem (draggable windows feeling very
laggy under software emulation). Verified: 5+ clean boots under real
KVM with no fault, all 27 QEMU smoke tests and all 4 host suites still
pass under the original TCG path, `-d int,cpu_reset` trace unchanged
under TCG.

## Context
This session's user-reported problem was that Milestone 31's
draggable windows felt very laggy in practice. Investigating why led to
testing under `-enable-kvm` (this machine's `/dev/kvm` is genuinely
accessible, confirmed, not assumed) as the likely real fix for the
underlying performance problem (`sys_fb_present()`'s own naive
per-pixel software blit loop, called up to 5 times per single mouse-move
event during a drag, run through TCG's software CPU interpretation).
Booting under KVM immediately produced a full, reproducible `#GP`
crash shortly after the fork/COW self-test — a fault TCG had never
once produced across this entire project's history. CLAUDE.md is
explicit that `-enable-kvm` must not be turned on without confirming
nested virtualization genuinely works — this ADR is that confirmation,
and the real bug that made it unsafe to do so before this fix existed.

## Decision

- **Diagnose with hard evidence, not the first plausible guess.** The
  `#GP`'s error code (`0x20`) decodes to GDT index 4
  (`USER_DATA_SELECTOR`) — the FIRST hypothesis (SS's own RPL bits
  missing, `0x20` instead of `0x23`) is actually indistinguishable from
  several OTHER possible causes just from the error code alone (its
  format masks out the low 3 bits, which is exactly where RPL lives)
  — a distinction confirmed by re-deriving the exact Intel SDM error
  code bit layout, not assumed. A defensive check added directly to
  `timer_tick_handler` (verifying CS/SS RPL consistency before every
  `iretq`) did NOT fire — ruling out the most common task-switch path
  entirely and preventing a wrong fix from being applied to the wrong
  place. Real, cited web research (Intel/AMD documentation via search)
  confirmed the G-bit/segment-limit fields are architecturally IGNORED
  for data segments in 64-bit mode, ruling out a second hypothesis
  before it could waste further effort.
- **Built a small flight recorder** (`kernel/sched/scheduler.c`'s
  `scheduler_record_switch_diag()`/`scheduler_dump_switch_diag()`) to
  capture the last 8 frames handed to `iretq` from EVERY path that can
  do so (`timer_tick_handler`, AND `exceptions.c`'s `isr_handler` for
  both its early-return cases: `#BP` resume and resolved COW `#PF`
  resume) — dumped automatically on a real `#GP`. This is what actually
  found the bug: the second-to-last recorded entry showed task 9
  (the fork/wait demo's parent) with a CORRECT `ss=0x23` at its
  synthetic entry frame, then, just 0x14 bytes into real execution (at
  its own COW write fault — `mov [child_pid], rax`, an entirely
  ordinary instruction, Milestone 21's own established, 10+-milestone-
  proven COW mechanism), the SAME task's hardware-captured frame showed
  `ss=0x20` — RPL bits gone. Every trap-frame `.ss` CONSTRUCTION site
  in `task.c` was individually re-verified correct (all three
  correctly use `USER_DATA_SELECTOR | 3`) — the corruption happens
  between a correct synthetic frame and the CPU's own later capture of
  a real fault frame for that same task, on real hardware only.
- **Fixed defensively, not by chasing the exact hypervisor/silicon
  mechanism to its root.** The precise reason KVM's captured `ss` loses
  its RPL bits specifically for a ring-3 COW `#PF` frame is deep
  VT-x/segment-descriptor-cache territory this ADR does not claim to
  have fully resolved. Instead: this kernel's OWN architecture
  guarantees only two possible SS values EVER exist (`KERNEL_DATA_SELECTOR`
  for CPL0, `USER_DATA_SELECTOR | 3` for CPL3 — enforced uniformly by
  all three of `task.c`'s trap-frame construction sites, no per-task
  variation ever). `trap_frame_fixup_ss()`
  (`kernel/arch/x86_64/trap_frame.h`) re-asserts the correct SS based
  on the frame's OWN `cs` (which never showed this corruption) right
  before every `iretq` — provably safe given that invariant, not a
  guess. This is the same class of fix real kernels use for analogous
  real-hardware SS-corruption classes around ring transitions (Linux's
  own workaround for AMD SYSRET leaving a stale/null SS is the same
  shape: don't trust a value hardware has just demonstrated can be
   unreliable here, reassert the invariant your own design guarantees).
- **Kept the flight recorder permanently**, not just for this
  investigation — the same "capture recent state before a fault, not
  just at it" technique is generically useful for any future fault
  this kernel can't yet resolve, a natural extension of CLAUDE.md's own
  safety rule 6.
- **`make run`/`make debug` now enable KVM conditionally**
  (`KVM_FLAG := $(shell test -r /dev/kvm -a -w /dev/kvm && echo -enable-kvm)`),
  checked at `make` invocation time rather than hardcoded — stays
  correct on a future machine/container without KVM access, and
  matches CLAUDE.md's own requirement to confirm (not assume) nested
  virtualization works before ever passing this flag.

## Rejected alternatives
- **Trusting the error code's RPL implication at face value** and
  fixing "the RPL bits are missing" without further verification.
  Rejected once the diagnostic check for exactly that condition did
  NOT catch the fault — would have been a wrong fix chasing a plausible
  but unconfirmed hypothesis, the opposite of CLAUDE.md's own "diagnose
  first" discipline.
- **Chasing the exact KVM/VT-x root cause to full resolution** before
  fixing anything. Rejected as disproportionate: the defensive fix is
  provably correct given this kernel's own SS-value invariant,
  regardless of the hypervisor's own internal reason for the
  corruption — further root-causing deep inside KVM/VT-x segment-cache
  behavior is out of scope for what this kernel needs to be correct.
- **Leaving `-enable-kvm` off permanently**, avoiding the bug by never
  triggering the code path that exposes it. Rejected — this would have
  left both a real, unfixed hardware-correctness bug (that would still
  bite on ACTUAL bare-metal hardware, the project's own long-term
  aspiration) and the actual performance problem the user reported.

## Verification
- Reproduced the `#GP` reliably under `-enable-kvm` before the fix (100%
  of attempts, always shortly after the fork/COW self-test).
- Flight recorder evidence (captured, not guessed): task 9's own frame
  sequence showing `ss=0x23` (synthetic, correct) → `ss=0x20`
  (real, captured, WRONG) two entries later, at a COW `#PF`.
- After the fix: 5 consecutive clean full boots under real KVM, every
  self-test passing, reaching the shell prompt every time, zero `#GP`
  faults.
- All twenty-seven QEMU smoke tests and all four host test suites
  re-verified passing under the ORIGINAL TCG path (unaffected — the fix
  is a no-op whenever the captured `ss` was already correct, which TCG
  always produces). `-d int,cpu_reset` trace under TCG: unchanged (1
  `#BP`, 3 `#PF`, zero kernel-caused resets). The SAME trace under KVM
  correctly shows 0 for both — expected, not a regression:
  `-d int` is a TCG-only tracing hook into QEMU's own software
  interrupt dispatch, which hardware-accelerated execution bypasses
  entirely by design.

## Known limitations (accepted for this milestone only)
The exact hypervisor/silicon mechanism producing the corrupted `ss`
capture is not fully understood — only defended against. If a FUTURE
fault ever shows a similarly corrupted CS (not just SS), the same
defensive-reassertion technique would need extending; nothing currently
monitors for that specific case, though the flight recorder kept
permanently by this ADR would at least surface it for investigation.
