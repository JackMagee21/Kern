#include <stddef.h>
#include <stdint.h>

#include "scheduler.h"
#include "task.h"
#include "../arch/x86_64/irq.h"
#include "../arch/x86_64/trap_frame.h"
#include "../arch/x86_64/tss.h"
#include "../arch/x86_64/syscall.h"
#include "../drivers/console.h"
#include "../drivers/pit.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
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
 *
 * Per-process address spaces: also arranges for CR3 to be reloaded from
 * current_task->pml4 on every switch, but only when it actually
 * changes -- reloading CR3 unconditionally would flush the entire TLB
 * on every single 100Hz tick even for kernel-thread-to-kernel-thread
 * switches, which is most of them, since they all share one address
 * space. The actual `mov cr3` does NOT happen here in C -- see
 * common_stub.inc's comment for why doing it here (as a first attempt
 * did, and triple-faulted on the very first real switch) is unsafe:
 * this function is still running on the OUTGOING task's stack, which
 * isn't guaranteed reachable under the INCOMING task's page tables.
 * scheduler_current_pml4/scheduler_target_pml4 are deliberately plain
 * (non-static) globals, read/written directly by common_stub.inc's
 * asm at the one point in the resume path that's actually safe.
 *
 * Process lifecycle (ADR 0010): the ready queue is doubly-linked
 * (next/prev), not singly, so a zombie task can be unlinked in O(1)
 * without a search -- task_t's next/prev fields and this queue's shape
 * are documented together in task.h. A task that calls
 * scheduler_exit_current() gets marked TASK_ZOMBIE and never runs
 * again; timer_tick_handler notices on the NEXT tick (the only point
 * it's ever examined), unlinks it from the ready queue, and pushes it
 * onto zombie_head -- a separate singly-linked chain (reusing the same
 * ->next field, now safe to repurpose since the task has left the
 * ready queue) that a dedicated reaper task drains.
 *
 * Actually freeing a zombie's resources can't happen inside
 * timer_tick_handler itself, for the same category of reason ADR
 * 0009's CR3-switch-timing bug exists: at the point a zombie is
 * detected, this handler is still running ON THAT TASK'S OWN kernel
 * stack, and its PML4 is still the active CR3 (both only actually
 * change later, in common_stub.inc, after this C function returns) --
 * freeing either out from under still-active execution/mappings would
 * be a use-after-free. The reaper is a separate, ordinary kernel
 * thread with its own stack and (since it's a kernel thread) the
 * kernel's own shared address space, so by the time it actually runs
 * and processes a zombie, that zombie's stack/PML4 are guaranteed to
 * no longer be in use -- reap_next()'s cli/sti section is what makes
 * zombie_head itself safe to share between the reaper (normal thread
 * context) and timer_tick_handler (interrupt context), per CLAUDE.md
 * safety rule 1.
 */

static task_t *current_task;
static task_t *zombie_head;
static uint64_t reaped_count;

uint64_t scheduler_current_pml4;
uint64_t scheduler_target_pml4;

static trap_frame_t *timer_tick_handler(trap_frame_t *frame)
{
    pit_tick();

    task_t *outgoing = current_task;
    outgoing->rsp = (uint64_t)frame;

    if (outgoing->next == outgoing) {
        panic("scheduler: cannot remove the only task in the ready queue");
    }
    current_task = outgoing->next;

    if (outgoing->state == TASK_ZOMBIE) {
        outgoing->prev->next = outgoing->next;
        outgoing->next->prev = outgoing->prev;
        outgoing->next = zombie_head; /* reused: outgoing has left the ready queue */
        zombie_head = outgoing;
    }

    tss_set_rsp0(current_task->kernel_stack_top);
    syscall_set_kernel_stack(current_task->kernel_stack_top);
    scheduler_target_pml4 = current_task->pml4;

    return (trap_frame_t *)current_task->rsp;
}

/* Pops the oldest pending zombie, or NULL if none are waiting. The
   only piece of scheduler state genuinely shared with an interrupt
   handler (zombie_head, written by timer_tick_handler) that isn't
   already interrupt-context-only, so this is the one place in the
   reaper's normal-context code that needs its own critical section. */
static task_t *reap_next(void)
{
    __asm__ volatile("cli");
    task_t *dead = zombie_head;
    if (dead != NULL) {
        zombie_head = dead->next;
    }
    __asm__ volatile("sti");
    return dead;
}

static void reaper_task(void)
{
    for (;;) {
        task_t *dead = reap_next();
        if (dead == NULL) {
            __asm__ volatile("hlt");
            continue;
        }

        vmm_destroy_address_space(dead->pml4);
        task_free_kernel_stack(dead->kernel_stack_base, dead->kernel_stack_top - dead->kernel_stack_base);
        console_write("[OK] process ");
        console_write_hex(dead->id);
        console_write(" exited and was reaped\n");
        kfree(dead);
        reaped_count++;
    }
}

void scheduler_exit_current(void)
{
    current_task->state = TASK_ZOMBIE;
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

uint64_t scheduler_reaped_count(void)
{
    return reaped_count;
}

void scheduler_init(void)
{
    task_t *bootstrap = (task_t *)kmalloc(sizeof(task_t));
    if (bootstrap == NULL) {
        panic("scheduler_init: kmalloc failed for the bootstrap task");
    }

    bootstrap->rsp = 0; /* filled in by timer_tick_handler the first time this context is preempted */
    bootstrap->kernel_stack_top = 0; /* never consulted: kernel_main runs at ring 0, RSP0 is only used for ring3->ring0 */
    bootstrap->kernel_stack_base = 0; /* never reaped -- the bootstrap task never exits */
    bootstrap->pml4 = vmm_current_pml4(); /* the kernel's own address space -- already active, no reload needed */
    bootstrap->state = TASK_READY;
    bootstrap->next = bootstrap; /* circular list of one, until scheduler_add_task grows it */
    bootstrap->prev = bootstrap;
    bootstrap->id = 0;

    current_task = bootstrap;
    scheduler_current_pml4 = bootstrap->pml4;
    scheduler_target_pml4 = bootstrap->pml4;
    tss_set_rsp0(0);
    syscall_set_kernel_stack(0);

    irq_register_handler(0, timer_tick_handler);

    /* The reaper is scheduler-internal infrastructure (process exit
       doesn't work without it), not demo content -- spawned here
       rather than left for kernel_main to remember to add. */
    scheduler_add_task(task_create(reaper_task));
}

void scheduler_add_task(task_t *task)
{
    task->state = TASK_READY;
    task->next = current_task->next;
    task->prev = current_task;
    current_task->next->prev = task;
    current_task->next = task;
}
