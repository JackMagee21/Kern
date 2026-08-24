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

/* Process lifecycle (ADR 0010): marks the CURRENTLY RUNNING task
   (i.e. the caller) TASK_ZOMBIE, enables interrupts, and halts forever
   -- never returns. The next timer tick unlinks it from the ready
   queue and hands it to the reaper task (spawned internally by
   scheduler_init()), which frees its address space/stacks/task_t once
   it's actually safe to (see scheduler.c's reaper_task comment for why
   that can't happen synchronously here). Only meaningful called from a
   ring-3 syscall (sys_exit) -- nothing calls this for a kernel thread,
   since none of them ever intend to stop running. */
void scheduler_exit_current(void) __attribute__((noreturn));

/* Total tasks the reaper has fully torn down so far -- lets a self-test
   wait for a specific number of exits to be reaped, the same
   observability pattern as syscall_get_count()/pit_get_ticks(). */
uint64_t scheduler_reaped_count(void);

#endif /* KERNEL_SCHED_SCHEDULER_H */
