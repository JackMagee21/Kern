#include <stddef.h>
#include <stdint.h>

#include "scheduler.h"
#include "task.h"
#include "../arch/x86_64/irq.h"
#include "../arch/x86_64/trap_frame.h"
#include "../arch/x86_64/tss.h"
#include "../arch/x86_64/syscall.h"
#include "../drivers/pit.h"
#include "../mm/heap.h"
#include "../panic.h"

/*
 * Round-robin preemptive scheduler. No priorities, no blocking -- every
 * task in the circular ready queue gets an equal turn, forced by the
 * timer whether or not it would voluntarily yield. Owns IRQ0 (registers
 * its own handler, which calls pit_tick() itself) rather than PIT
 * driving this directly, keeping kernel/drivers/pit.c a plain hardware
 * driver with no scheduling-policy dependency -- see ADR 0006.
 *
 * Milestone 7: also updates TSS.RSP0 and the syscall entry stack on
 * every switch, to whichever task is now current. Necessary because a
 * ring-3 task's kernel_stack_top is its OWN dedicated stack (see
 * task.h) -- if RSP0/the syscall stack pointer weren't kept in sync
 * with current_task, a second ring-3 task being interrupted or making
 * a syscall while a FIRST one is still preempted (its saved context
 * sitting on ITS kernel stack, waiting for its next turn) would land on
 * the wrong stack and corrupt it.
 */

static task_t *current_task;

static trap_frame_t *timer_tick_handler(trap_frame_t *frame)
{
    pit_tick();

    current_task->rsp = (uint64_t)frame;
    current_task = current_task->next;

    tss_set_rsp0(current_task->kernel_stack_top);
    syscall_set_kernel_stack(current_task->kernel_stack_top);

    return (trap_frame_t *)current_task->rsp;
}

void scheduler_init(void)
{
    task_t *bootstrap = (task_t *)kmalloc(sizeof(task_t));
    if (bootstrap == NULL) {
        panic("scheduler_init: kmalloc failed for the bootstrap task");
    }

    bootstrap->rsp = 0; /* filled in by timer_tick_handler the first time this context is preempted */
    bootstrap->kernel_stack_top = 0; /* never consulted: kernel_main runs at ring 0, RSP0 is only used for ring3->ring0 */
    bootstrap->id = 0;
    bootstrap->next = bootstrap; /* circular list of one, until scheduler_add_task grows it */

    current_task = bootstrap;
    tss_set_rsp0(0);
    syscall_set_kernel_stack(0);

    irq_register_handler(0, timer_tick_handler);
}

void scheduler_add_task(task_t *task)
{
    task->next = current_task->next;
    current_task->next = task;
}
