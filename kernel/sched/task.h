#ifndef KERNEL_SCHED_TASK_H
#define KERNEL_SCHED_TASK_H

#include <stdint.h>

/* rsp points at a trap_frame_t once the task has a saved context --
   either a synthetic one task_create() built (never yet run) or a real
   one left by the timer preempting it mid-execution. next forms a
   circular ready-queue (kernel/sched/scheduler.c), round robin, no
   priorities/blocking yet -- nothing needs them at this milestone. */
typedef struct task {
    uint64_t rsp;
    struct task *next;
    uint32_t id;
} task_t;

/* 16KiB per task, fixed. CLAUDE.md: know the stack size for every
   context -- these are kernel threads (single shared address space, no
   ring 3 yet, that's Milestone 7) doing small, non-recursive work, so a
   single fixed size for all of them is enough for now. */
#define TASK_STACK_SIZE (16u * 1024u)

/* Allocates a stack (via kmalloc) and builds a synthetic trap frame on
   it so it can be resumed through the exact same iretq path a real
   interrupt uses (common_stub.inc / kernel/sched/scheduler.c) the first
   time the scheduler switches to it. entry must never return -- there
   is nowhere to return to (no process exit path exists yet). Panics if
   the heap can't satisfy the allocation. */
task_t *task_create(void (*entry)(void));

#endif /* KERNEL_SCHED_TASK_H */
