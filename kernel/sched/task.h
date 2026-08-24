#ifndef KERNEL_SCHED_TASK_H
#define KERNEL_SCHED_TASK_H

#include <stdint.h>

/* rsp points at a saved context (trap_frame_t for a kernel task,
   syscall/interrupt-preempted or synthetic either way) -- either a
   synthetic one a task_create* function built (never yet run) or a
   real one left by the timer preempting it mid-execution. next forms a
   circular ready-queue (kernel/sched/scheduler.c), round robin, no
   priorities/blocking yet -- nothing needs them at this milestone.

   kernel_stack_top (Milestone 7): the stack TSS.RSP0 must point to
   while this task is current, so that if IT specifically is the one
   interrupted from ring 3, the CPU lands the trap frame on ITS OWN
   dedicated stack -- not a stack some OTHER task's still-pending saved
   context is sitting on. For a ring-0 task this is never actually
   consulted (same-privilege interrupts don't switch stacks), so it's
   just set to the task's own stack as a harmless default. */
typedef struct task {
    uint64_t rsp;
    uint64_t kernel_stack_top;
    struct task *next;
    uint32_t id;
} task_t;

/* 16KiB per task, fixed. CLAUDE.md: know the stack size for every
   context -- these are small, non-recursive demo/self-test tasks, so a
   single fixed size for all of them is enough for now. */
#define TASK_STACK_SIZE (16u * 1024u)

/* Allocates a kernel-mode stack (via kmalloc) and builds a synthetic
   trap frame on it so it can be resumed through the exact same iretq
   path a real interrupt uses (common_stub.inc / scheduler.c) the first
   time the scheduler switches to it. Runs at ring 0, same address
   space as the kernel. entry must never return -- there is nowhere to
   return to (no process exit path exists yet). Panics if the heap
   can't satisfy the allocation. */
task_t *task_create(void (*entry)(void));

/* Milestone 7: builds the one ring-3 demo task. Unlike task_create(),
   entry isn't a C function pointer -- it's kernel/sched/user_demo.asm's
   hand-written, position-independent blob (see ADR 0007 for why it
   can't be a normal C function: mcmodel=kernel code lives inside the
   kernel's own supervisor-only 2MiB pages, and there's no way to mark
   just one C function's containing page user-accessible without
   exposing everything else sharing that huge page). Maps the demo
   code, a fresh user-accessible stack, AND a separate, NOT
   user-accessible kernel-mode stack (for TSS.RSP0 / syscall entry) into
   dedicated virtual regions. Panics on any allocation/mapping failure. */
task_t *task_create_user(void);

#endif /* KERNEL_SCHED_TASK_H */
