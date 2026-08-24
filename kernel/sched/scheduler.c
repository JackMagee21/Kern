#include <stddef.h>
#include <stdint.h>

#include "scheduler.h"
#include "task.h"
#include "../arch/x86_64/irq.h"
#include "../arch/x86_64/trap_frame.h"
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
 */

static task_t *current_task;

static trap_frame_t *timer_tick_handler(trap_frame_t *frame)
{
    pit_tick();

    current_task->rsp = (uint64_t)frame;
    current_task = current_task->next;
    return (trap_frame_t *)current_task->rsp;
}

void scheduler_init(void)
{
    task_t *bootstrap = (task_t *)kmalloc(sizeof(task_t));
    if (bootstrap == NULL) {
        panic("scheduler_init: kmalloc failed for the bootstrap task");
    }

    bootstrap->rsp = 0; /* filled in by timer_tick_handler the first time this context is preempted */
    bootstrap->id = 0;
    bootstrap->next = bootstrap; /* circular list of one, until scheduler_add_task grows it */

    current_task = bootstrap;

    irq_register_handler(0, timer_tick_handler);
}

void scheduler_add_task(task_t *task)
{
    task->next = current_task->next;
    current_task->next = task;
}
