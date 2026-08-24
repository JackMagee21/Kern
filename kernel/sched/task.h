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
   just set to the task's own stack as a harmless default.

   pml4 (per-process address spaces): the top-level page table CR3
   should hold while this task is current. Every kernel thread shares
   the SAME value (the kernel's own, original address space --
   task_create() just reads whatever's currently active) since they're
   part of the kernel itself, not isolated/untrusted; only
   task_create_user() processes get a genuinely private one from
   vmm_create_address_space(). The scheduler reloads CR3 from this
   field on every switch, but only when it actually changes.

   kernel_stack_base (process lifecycle, ADR 0010): the pointer
   kmalloc() actually returned for this task's kernel-mode stack --
   kernel_stack_top alone isn't enough to kfree() it correctly, since
   it points at the far END of the allocation, not its start.

   state/next/prev (process lifecycle, ADR 0010): a task is either
   TASK_READY, live in the scheduler's doubly-linked circular ready
   queue (next/prev both meaningful), or TASK_ZOMBIE, unlinked from
   that queue and instead singly-linked into the reaper's pending list
   (next reused as that chain's link -- prev is no longer meaningful
   once a task leaves the ready queue, so there's no need for a second
   field just for the zombie chain too). See kernel/sched/scheduler.c
   for why actually freeing a zombie's resources (kernel_stack_base,
   pml4, the task_t itself) has to be deferred to a separate reaper
   task rather than happening as soon as a task exits. */
typedef enum {
    TASK_READY,
    TASK_ZOMBIE,
} task_state_t;

typedef struct task {
    uint64_t rsp;
    uint64_t kernel_stack_top;
    uint64_t kernel_stack_base;
    uint64_t pml4;
    task_state_t state;
    struct task *next;
    struct task *prev;
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

/* Builds one ring-3 demo process, in its OWN address space
   (vmm_create_address_space()) -- callable more than once; each call
   is a genuinely independent process, not a second handle onto the
   same one. Unlike task_create(), entry isn't a C function pointer --
   it's kernel/sched/user_demo.asm's hand-written, position-independent
   blob (see ADR 0007 for why it can't be a normal C function:
   mcmodel=kernel code lives inside the kernel's own supervisor-only
   2MiB pages, and there's no way to mark just one C function's
   containing page user-accessible without exposing everything else
   sharing that huge page). The underlying physical code page is safe
   to share read-only across every process's address space (same as
   how real OSes share program text between instances of the same
   program) -- only the stack is private per process, fresh physical
   frames each call. Also maps a separate, NOT user-accessible
   kernel-mode stack (for TSS.RSP0 / syscall entry -- see task_t's
   pml4/kernel_stack_top doc comment). Panics on any allocation/mapping
   failure. */
task_t *task_create_user(void);

#endif /* KERNEL_SCHED_TASK_H */
