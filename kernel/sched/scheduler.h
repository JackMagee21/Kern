#ifndef KERNEL_SCHED_SCHEDULER_H
#define KERNEL_SCHED_SCHEDULER_H

#include "task.h"
#include "../arch/x86_64/trap_frame.h"

/* Creates the bootstrap task representing kernel_main's own
   already-running context (its saved rsp is filled in lazily, the
   first time the timer preempts it) and registers the preemption
   handler on IRQ0. Must run after idt_init()/pic_remap()/pit_init(),
   but does NOT itself unmask IRQ0 or call sti -- callers still do that
   explicitly once every task they want in the rotation has been added
   (matches Milestone 5's existing sequencing in kernel_main). */
void scheduler_init(void);

/* Adds task to the round-robin ready queue. */
void scheduler_add_task(task_t *task);

/* Process lifecycle (ADR 0010; exit_code added Milestone 18, ADR 0018):
   records exit_code on the CURRENTLY RUNNING task (i.e. the caller),
   marks it TASK_ZOMBIE, enables interrupts, and halts forever -- never
   returns. The next timer tick unlinks it from the ready queue and
   hands it to the reaper task (spawned internally by scheduler_init()),
   which frees its address space/stacks once it's actually safe to (see
   scheduler.c's reaper_task comment for why that can't happen
   synchronously here); the task_t struct itself is only freed
   immediately if the task has no parent (parent_id == 0) -- otherwise
   it's held (exit_code intact) for scheduler_try_wait() to collect.
   Only meaningful called from a ring-3 syscall (sys_exit) -- nothing
   calls this for a kernel thread, since none of them ever intend to
   stop running. */
void scheduler_exit_current(uint64_t exit_code) __attribute__((noreturn));

/* Total tasks the reaper has fully torn down (address space/stacks
   freed) so far -- lets a self-test wait for a specific number of
   exits to be reaped, the same observability pattern as
   syscall_get_count()/pit_get_ticks(). Incremented regardless of
   whether the task_t itself was freed immediately or is still pending
   collection via scheduler_try_wait() -- it tracks RESOURCE reclaiming
   (what a frame-leak self-test cares about), not task_t lifetime. */
uint64_t scheduler_reaped_count(void);

/* The task currently executing on this (the only) CPU -- e.g. what a
   syscall handler should treat as "the caller" (kernel/arch/x86_64/
   syscall.c's sys_fork/sys_wait). */
task_t *scheduler_current_task(void);

/* Milestone 18 (ADR 0018): non-blocking wait, a single one-shot check --
   if a task with parent_id == caller_id (and, unless target_pid is 0,
   id == target_pid) has exited and had its resources reclaimed but not
   yet been collected, unlinks and frees its task_t, writes its exit
   code to *out_exit_code (if out_exit_code != NULL), and returns its
   pid. Returns 0 (never a valid pid -- see task.h) and leaves
   *out_exit_code untouched if no matching task is ready yet. Does NOT
   block itself -- sys_wait (kernel/arch/x86_64/syscall.c) is what turns
   repeated calls to this into a genuinely blocking wait (Milestone 20,
   ADR 0020, via its own sti/hlt/cli retry loop, not yet rebuilt on top
   of scheduler_block_current()/scheduler_wake() below -- see ADR 0025's
   Known limitations for why that rewiring was deliberately left for a
   later milestone). */
uint32_t scheduler_try_wait(uint32_t caller_id, uint32_t target_pid, uint64_t *out_exit_code);

/* Milestone 25 (ADR 0025): a general blocking primitive, generalizing
   the one-off sti/hlt/cli retry loop sys_wait has used since Milestone
   20 into a real TASK_BLOCKED state. Marks the CURRENTLY RUNNING task
   (the caller) TASK_BLOCKED and re-enables interrupts -- the NEXT timer
   tick notices the state change and unlinks it from the ready queue
   (same mechanism/timing as TASK_ZOMBIE's own unlink, see
   timer_tick_handler), so a blocked task consumes ZERO further
   scheduler turns until scheduler_wake() explicitly relinks it, unlike
   sys_wait's old polling design where the caller stayed TASK_READY and
   kept getting (wasted) turns the whole time it was waiting. Does not
   return until scheduler_wake(scheduler_current_task()) has been called
   by someone else -- callable only from a task's own normal execution
   context (e.g. mid-syscall, matching sys_wait's existing precedent),
   NOT from interrupt-handler context. Contract: MUST be called with
   interrupts already disabled (mid-syscall, IF=0, is the only context
   this kernel calls it from so far) and ALWAYS returns with interrupts
   disabled again -- the same in/out invariant sys_wait's own retry loop
   already had, just factored out. */
void scheduler_block_current(void);

/* Milestone 25 (ADR 0025): wakes `task` if it is currently
   TASK_BLOCKED (a no-op otherwise -- safe to call speculously, e.g. "in
   case something is waiting," without the caller having to track
   whether it actually is) by marking it TASK_READY and relinking it
   into the ready queue next to the CURRENTLY running task (reuses
   scheduler_add_task()'s own insertion logic exactly). Unlike
   scheduler_block_current(), this IS safe to call from either a normal
   task context OR interrupt-handler context: internally it saves and
   restores the caller's OWN prior interrupt-flag state (`pushfq`/
   `popfq` around a `cli`-protected critical section) rather than
   unconditionally forcing interrupts on afterward -- forcing them on
   would be a real bug if the caller turns out to already be running
   inside an interrupt handler (IF=0 for the handler's own entire
   duration, by design, since every exception/IRQ gate in this kernel is
   an interrupt gate, idt.c) that hasn't finished yet. No current caller
   needs the interrupt-handler-context case, but a future IRQ-driven
   wake (e.g. keyboard input unblocking a waiting reader) will, so this
   is designed for both from the start rather than needing a second,
   redundant variant later. */
void scheduler_wake(task_t *task);

/* Milestone 26 (ADR 0026): a general pid -> task_t* lookup, usable
   regardless of whether the target is TASK_READY or TASK_BLOCKED --
   sys_ipc_send (kernel/arch/x86_64/syscall.c) needs to find a message's
   destination task purely from a pid a caller supplied, and unlike
   every OTHER consumer of a task_t* so far (scheduler_wake()'s own
   callers, task_fork()'s parent access), it has no other way to get
   one: the sender and receiver aren't necessarily parent/child (the
   GUI arc's window server and its client apps are siblings, both
   spawned directly by kernel_main), and a BLOCKED task (e.g. already
   waiting on sys_ipc_recv for a PREVIOUS message) is, by Milestone 25's
   own design, unlinked from the ready queue entirely -- not searchable
   there. Backed by a small fixed-capacity registry
   (scheduler_register_task()/scheduler_unregister_task(), called once
   each from every task creation site and every task teardown site
   respectively) rather than growing an already-existing list's role,
   since neither the ready queue (only READY tasks) nor collected_head
   (only reaped-but-uncollected zombies) covers "any live task,
   whatever its current state." Returns NULL if no live task has that
   id. */
task_t *scheduler_find_task(uint32_t id);

/* Registers/unregisters `task` in the pid lookup registry above.
   MUST be called exactly once per task, at creation (before the task
   could ever plausibly be an IPC target) and again at the point its
   task_t is actually freed (kfree()'d) -- never left registered past
   that point, or a future id lookup could return a dangling pointer.
   Panics if the registry is ever full (a fixed-capacity array, sized
   generously for this kernel's current scale -- see scheduler.c) rather
   than silently failing to track a live task. */
void scheduler_register_task(task_t *task);
void scheduler_unregister_task(task_t *task);

/* Milestone 32 (ADR 0032): records one entry (task id + frame's
   rip/cs/ss/rsp/rflags) into a small ring buffer;
   scheduler_dump_switch_diag() prints its contents. Callable from any
   frame-returning path (timer_tick_handler AND exceptions.c's
   isr_handler) to build a flight recorder of recent task-switch state
   -- exceptions.c dumps it on a real #GP. Originally built to root-
   cause a real, hardware-only SS-corruption bug (see
   kernel/arch/x86_64/trap_frame.h's trap_frame_fixup_ss()); kept
   permanently since the same technique is generically useful for any
   future fault this kernel can't yet resolve. */
void scheduler_record_switch_diag(uint32_t task_id, const trap_frame_t *frame);
void scheduler_dump_switch_diag(void);

#endif /* KERNEL_SCHED_SCHEDULER_H */
