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

#endif /* KERNEL_SCHED_SCHEDULER_H */
