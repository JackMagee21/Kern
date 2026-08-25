# ADR 0026: IPC message passing and shared memory

## Status
Accepted and verified — `make run` boots the real ISO; two genuinely
isolated processes (`kernel/user/ipc_sender.c`/`ipc_receiver.c`)
exchange a real message and a real block of shared physical memory,
verified via a known pattern written by one side and read back
correctly by the other through a completely independent virtual
address. `sys_ipc_recv`'s blocking path is verified as genuinely
exercised, deterministically (not by luck), via process creation order
reasoned through explicitly rather than assumed. **Three real bugs
were found and fixed during this milestone** — see Verification for
the full diagnostic trail, each one caught by an actual observed
symptom, not guessed in advance. All twenty-four pre-existing smoke
tests plus the new `test_ipc_shm_selftest.sh` pass; all four host test
suites pass. `-d int,cpu_reset` trace unchanged from Milestone 25.
Booted 5 times back to back with identical shape every time after the
fixes.

## Context
Third step of the GUI arc (`Desktop.md`), following the userspace C
runtime (Milestone 24) and the general blocking/wake primitive
(Milestone 25). A display server and its client apps are genuinely
separate, isolated processes (Milestone 9's whole point) — they need a
way to exchange both small control messages and bulk pixel data
without copying through a filesystem that doesn't exist. This is also
the concrete need that finally motivates SOME form of the
long-deferred VMA-tracking item (`future.md`) — though, as the Decision
below explains, not the full general mechanism.

## Decision

- **A general `pid -> task_t*` lookup was needed, and didn't exist.**
  `sys_ipc_send` addresses its destination by pid, and unlike every
  prior consumer of a `task_t*` (parent/child via `parent_id`,
  `scheduler_wake()`'s callers holding a direct pointer already), the
  destination here could be `TASK_BLOCKED` — by Milestone 25's own
  design, unlinked from the ready queue and not searchable there. Added
  `scheduler_register_task()`/`scheduler_unregister_task()`/
  `scheduler_find_task()` (`kernel/sched/scheduler.c`): a small,
  fixed-capacity (`MAX_LIVE_TASKS = 64`) flat registry, linear-scanned
  (ids are never recycled, so indexing directly by id would grow
  unboundedly over a long uptime — a bounded array of live SLOTS is
  simpler and correct at this kernel's scale). Registration happens at
  each task-creation site directly (`task_create()`/
  `task_create_user_image()`/`task_fork()`, plus the bootstrap task in
  `scheduler_init()`) — deliberately NOT inside `scheduler_add_task()`,
  since that function is ALSO reused by `scheduler_wake()` to relink an
  already-registered blocked task, which must not register a second
  time.
- **`ipc_message_t`: one shared struct definition** (`kernel/ipc/
  ipc_message.h`), included unmodified by both kernel-side code
  (`kernel/ipc/msgqueue.c`) and userspace runtime code (`kernel/user/
  rt/syscall.h`) — a struct LAYOUT mismatch between the two sides would
  silently corrupt data, unlike the syscall NUMBERS (already safely
  duplicated as plain integers on each side since Milestone 24 — a
  mismatch there would at worst misdispatch). Fixed-size, fixed-field-
  count (`IPC_MSG_FIELDS = 4`) messages only — no variable-length
  payload support, since nothing this milestone's own consumers send
  needs one (a real protocol is future work for whichever milestone
  actually defines one).
- **A small, fixed-capacity per-task inbox, embedded directly in
  `task_t`** (`ipc_inbox[IPC_INBOX_CAPACITY]` + head/tail, `task.h`) —
  every task gets one, kernel thread or ring-3 process, at negligible
  cost (320 bytes). `ipc_send()` (`kernel/ipc/msgqueue.c`) pushes and
  unconditionally calls `scheduler_wake(dest)` — a no-op per that
  function's own contract if `dest` isn't currently blocked, so there's
  no separate "is anyone waiting" check needed. `sender_pid` is
  populated by the KERNEL (`sys_ipc_send`, `syscall.c`), never trusted
  from whatever the caller's own struct contained — the same
  "don't trust a user-supplied identity claim" stance this codebase
  already applies elsewhere.
- **Shared memory: named objects, NOT general VMA tracking.**
  `kernel/ipc/shm.c` is a small, fixed-capacity (16 objects, 1MiB/
  process budget) table of `{size, frame list}` — deliberately narrower
  than a real per-process memory map with arbitrary regions/
  permissions. Built because THIS concrete need (a client and a
  display server sharing a pixel buffer) exists now, not because VMAs
  themselves were needed for their own sake (CLAUDE.md: don't build for
  hypothetical future requirements) — `future.md`'s own long-deferred
  VMA item stays deferred, its actual trigger turned out to be
  narrower than expected.
- **Cleanup reuses `pmm.h`'s EXISTING refcounting (Milestone 21's COW
  mechanism) instead of a new "destroy" API.** Every mapper's own
  mapping is `VMM_FLAG_OWNED` in ITS OWN address space; an ordinary
  process exit (`vmm_destroy_address_space()`, unchanged since ADR
  0010) already drops exactly one reference per frame it mapped — the
  physical memory is only actually freed once EVERY mapper has exited,
  the identical "last reference frees it" pattern COW fork already
  proved correct. No new cleanup code was needed at all — see
  Verification for the real bug this reuse's first draft still managed
  to have.
- **A per-task VA bump allocator for shm mappings** (`task->shm_next_va`,
  a new `task_t` field, initialized to `SHM_VIRT_BASE` — the ~2MiB gap
  between the ELF image's own code and the user stack that no other
  convention currently claims), not a single kernel-wide counter — each
  process independently tracks its own bump offset, so one process's
  own mapping count can never affect where ANOTHER process's next
  mapping lands in its own, independent address space. `task_fork()`'s
  child does NOT reset this to `SHM_VIRT_BASE` — see Verification for
  why that would have been a real bug, caught before ever booting.

## Rejected alternatives
- **A general VMA subsystem** (arbitrary regions, arbitrary
  permissions, per-process memory map). Rejected for this milestone —
  see Decision; the concrete need was narrower than the general
  mechanism `future.md` had speculated about.
- **A separate explicit `shm_destroy()` syscall.** Rejected — reusing
  `pmm`'s existing refcounting means ordinary process exit already
  provides correct, automatic cleanup with zero new code; an explicit
  destroy call would just be redundant machinery for the mainline case
  and would need its own careful "what if a mapper still holds it"
  semantics a plain refcount already handles for free.
- **argv/envp for pid discovery**, so `ipc_sender.c` could learn the
  receiver's pid without a kernel-injected bootstrap message. Rejected
  as out of THIS milestone's scope (ADR 0024's own Known limitations
  already flagged no argv/envp yet); `kernel_main` acting as a trusted
  "init" that hands out an initial pid via a direct, kernel-side
  `ipc_send()` call is a smaller, sufficient mechanism for this
  milestone's actual need.

## Verification
- `make run` (real toolchain) boots and prints every Milestone 1-25
  marker unchanged, plus the new demo's creation/success markers, in
  the correct sequence, ending with `[OK] ipc self-test passed,
  sys_ipc_recv genuinely blocked (0x1 turns)...`.
- **Three real bugs were found and fixed, each diagnosed from an actual
  observed symptom, not guessed:**
  1. **`ipc_sender.c` checked the wrong success/failure convention for
     `sys_ipc_send`.** The kernel returns 0 on success (no other
     meaningful value to report) and `(uint64_t)-1` on failure — but the
     demo's own first draft checked `== 0` as failure, backwards. Caught
     on the first real boot: the sender printed its own `[FAIL]`
     message and returned early, yet the receiver STILL reported a
     correct pattern match — a genuine contradiction that pointed
     straight at the send having actually succeeded despite the
     userspace logic misinterpreting its own return value. Fixed by
     correcting the check (and the runtime header's own doc comment,
     which had documented the same inverted convention).
  2. **A real frame leak**, caught by the process-lifecycle self-test's
     own exact-baseline check. `shm_create()`'s `pmm_alloc_frame()`
     already gives each frame an implicit refcount of 1 (the same "one
     implicit owner" every `pmm_alloc_frame()` caller gets); the first
     draft of `shm_map()` ALSO unconditionally called
     `pmm_frame_addref()` on every mapper, including the first —
     leaving a frame with refcount 2 backing only ONE real mapping, so
     that mapper's eventual exit only ever brought it back to 1, never
     0. Fixed by tracking whether a given object has had any successful
     `shm_map()` call yet (`shm_object_t::has_mapper`) and skipping the
     addref for the first one — the same "the pre-existing reference is
     untouched, only the NEW one gets counted" split
     `vmm_fork_cow_page()` already established for COW (ADR 0021).
  3. **`sys_ipc_recv_block_count` stayed 0** — `kernel_main`'s own
     self-test panicked. Root cause: `scheduler_add_task()` inserts each
     new task immediately after `current_task`, which stays the
     bootstrap task throughout ALL of `kernel_main`'s own setup code
     (interrupts are off, nothing can preempt it yet) — meaning the
     LAST task added ends up scheduled FIRST once preemption begins.
     The original code created the receiver BEFORE the sender,
     expecting the receiver to run (and block) first; the actual effect
     was the opposite — the sender ran first, delivered its message,
     and the receiver's own first check found it already waiting,
     never exercising the blocking path this self-test exists to
     prove. Fixed by swapping creation order (sender first, receiver
     last) — now deterministic BY CONSTRUCTION, not by luck, since
     nothing else can modify the ready queue during `kernel_main`'s
     interrupts-off setup phase.
  4. **A latent bug caught in review, before ever booting**: `task_fork()`'s
     first draft reset a forked child's `shm_next_va` to `SHM_VIRT_BASE`.
     But `task_fork()`'s existing COW-sharing walk
     (`vmm_for_each_user_page()`) doesn't distinguish which subsystem
     created a mapping — any shm object the PARENT already had mapped
     gets COW-shared into the child at the SAME virtual addresses,
     exactly like its stack or code does. Resetting the child's bump
     pointer to the region's base would make a LATER `shm_map()` call in
     the child try to reuse an address already occupied by an inherited
     mapping, hitting `shm_map()`'s own "fresh VA range" panic. Fixed by
     copying `parent->shm_next_va` instead — found by reasoning through
     what the EXISTING, unmodified fork mechanism would actually do
     with the new field, before ever running it.
- `-d int,cpu_reset` trace across a full boot (after all fixes):
  unchanged from Milestone 25 (1 `#BP`, 3 `#PF`, zero double-fault/
  reset) — this milestone's new code paths never fault.
- `tests/qemu/test_ipc_shm_selftest.sh` (new): checks every marker,
  absence of both demo programs' own `[FAIL]` paths, real sequencing,
  the exact reap count (7, up from 5 — `test_exec_selftest.sh`/
  `test_fork_wait_selftest.sh`/`test_process_lifecycle_selftest.sh` all
  needed this same assertion updated), and the frame-leak self-test.
  All twenty-four earlier smoke tests and all four host test suites
  re-verified passing. Booted 5 times back to back after the fixes —
  identical shape (including the exact block count, 1) every time.

## Known limitations (accepted for this milestone only)
Fixed-size, fixed-field-count messages only (`IPC_MSG_FIELDS = 4`) — no
variable-length payload. `ipc_send()` drops silently if the
destination's inbox is full (matching `mouse.c`'s own lossy-by-design
event-queue contract) — a sender blocking on a full inbox would need a
second wait-queue this milestone doesn't build. Shared-memory objects
are capped at 1MiB per process and 16 objects total, fixed-capacity,
generous for this milestone's actual needs but not general. No
priority/fairness policy for multiple tasks blocked on the same
resource. `sys_ipc_recv` has no non-blocking variant (always blocks) —
nothing this milestone needs one yet.
