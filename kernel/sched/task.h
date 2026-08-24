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

   kernel_stack_base (process lifecycle, ADR 0010; guard pages, ADR
   0012): the virtual address of the BOTTOM of this task's kernel-mode
   stack (kernel_stack_top points at the far END of the allocation, not
   its start) -- task_free_kernel_stack(kernel_stack_base,
   kernel_stack_top - kernel_stack_base) is how the reaper actually
   frees it. Not a kmalloc() pointer: since ADR 0012, every kernel-mode
   stack is its own dedicated, page-mapped VA region with a guard page
   below it (kernel/sched/task.c's alloc_kernel_stack()), not heap
   memory.

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

/* Allocates a dedicated, guard-paged kernel-mode stack (ADR 0012) and
   builds a synthetic trap frame on it so it can be resumed through the
   exact same iretq path a real interrupt uses (common_stub.inc /
   scheduler.c) the first time the scheduler switches to it. Runs at
   ring 0, same address space as the kernel. entry must never return --
   nothing calls sys_exit-equivalent teardown for a kernel thread; only
   ring-3 processes (task_create_user()) currently ever exit. Panics on
   any allocation/mapping failure. */
task_t *task_create(void (*entry)(void));

/* Frees a kernel-mode stack allocated by task_create()/
   task_create_user() (base/size = task_t::kernel_stack_base /
   kernel_stack_top - kernel_stack_base): looks up and pmm_free_frame()s
   every mapped frame in [base, base+size), then unmaps each page. The
   guard page below is never mapped in the first place, so there's
   nothing to free there. Only ever called by the reaper
   (kernel/sched/scheduler.c), once a task is safely no longer using
   its own stack -- see ADR 0010. */
void task_free_kernel_stack(uint64_t base, uint64_t size);

/* Builds one ring-3 process, in its OWN address space
   (vmm_create_address_space()) -- callable more than once; each call is
   a genuinely independent process, not a second handle onto the same
   one. Since Milestone 17 (ADR 0017), the process's code/data/bss come
   from parsing and mapping a REAL embedded ELF64 executable
   (kernel/mm/elf_loader.c, kernel/user/hello.asm) rather than Milestone
   7-16's single hand-written, position-independent blob (see ADR 0007
   for why a normal compiled C function couldn't be used directly:
   mcmodel=kernel code lives inside the kernel's own supervisor-only
   2MiB pages) mapped read-only and shared across every process. Every
   PT_LOAD segment now gets its own private, freshly allocated frame(s)
   per process instead -- only the stack was ever private before. Also
   maps a separate, NOT user-accessible kernel-mode stack (for TSS.RSP0
   / syscall entry -- see task_t's pml4/kernel_stack_top doc comment).
   Panics on any allocation/mapping failure, or if the embedded ELF
   image itself fails validation (would mean the build is broken, not
   bad runtime input). */
task_t *task_create_user(void);

#endif /* KERNEL_SCHED_TASK_H */
