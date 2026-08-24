#ifndef KERNEL_SCHED_SCHEDULER_H
#define KERNEL_SCHED_SCHEDULER_H

#include "task.h"

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

/* Milestone 18 (ADR 0018): non-blocking wait. If a task with
   parent_id == caller_id (and, unless target_pid is 0, id == target_pid)
   has exited and had its resources reclaimed but not yet been
   collected, unlinks and frees its task_t, writes its exit code to
   *out_exit_code (if out_exit_code != NULL), and returns its pid.
   Returns 0 (never a valid pid -- see task.h) and leaves *out_exit_code
   untouched if no matching task is ready yet; the caller (sys_wait) is
   expected to poll, see sys_wait's own doc comment for why this can't
   block instead. */
uint32_t scheduler_try_wait(uint32_t caller_id, uint32_t target_pid, uint64_t *out_exit_code);

#endif /* KERNEL_SCHED_SCHEDULER_H */
