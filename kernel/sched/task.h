#ifndef KERNEL_SCHED_TASK_H
#define KERNEL_SCHED_TASK_H

#include <stdint.h>

#include "../arch/x86_64/syscall.h" /* syscall_frame_t -- task_fork()'s parent_frame parameter, Milestone 18 */

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
   task rather than happening as soon as a task exits.

   parent_id/exit_code (Milestone 18, ADR 0018, fork/wait): parent_id
   is 0 for any task NOT created via task_fork() (every kernel thread,
   and every task_create_user_image() process spawned directly by
   kernel_main) -- 0 is a safe "no parent" sentinel since real task ids
   start at 1 (id 0 is reserved for the bootstrap task, which never
   forks). A parent_id != 0 task's task_t is NOT freed immediately when
   the reaper reclaims its resources -- it's held (this field plus
   exit_code plus id) on a separate collected-but-uncollected chain
   (scheduler.c's collected_head) until scheduler_try_wait() is called
   with a matching caller_id/target_pid. exit_code is set by
   scheduler_exit_current() right before a task becomes TASK_ZOMBIE;
   meaningless before that.

   saved_user_rsp (Milestone 20, ADR 0020, blocking wait): this task's
   own user-mode RSP at the moment of whichever syscall it is CURRENTLY
   executing -- meaningless while the task isn't mid-syscall. Used to
   be a single bare global in syscall_entry.asm, safe only because
   every syscall ran fully non-preemptible (ADR 0007); once sys_wait
   could block WITH interrupts enabled, a second, unrelated task's own
   syscall could enter and exit while the first sat blocked, and a
   single shared global would get clobbered. Moved here (one slot per
   task) plus a scheduler-maintained indirection pointer
   (syscall_set_user_rsp_slot(), updated on every context switch in
   scheduler.c's timer_tick_handler, the exact same per-task
   redirection pattern already used for TSS.RSP0/syscall_kernel_rsp)
   so a blocked task's own value survives arbitrarily many OTHER tasks'
   syscalls happening while it waits. */
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
    uint32_t parent_id;
    uint64_t exit_code;
    uint64_t saved_user_rsp;
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
   (vmm_create_address_space()), loading and mapping image_start..
   image_end as a real ELF64 executable (kernel/mm/elf_loader.c) --
   callable more than once, with any embedded image, each call a
   genuinely independent process. parent_id is set to 0 (orphan --
   nothing will ever scheduler_try_wait() for this process; matches
   every call kernel_main makes directly). Every PT_LOAD segment gets
   its own private, freshly allocated frame(s) (see ADR 0017), with
   permissions derived from the segment's own p_flags. Also maps a
   separate, NOT user-accessible kernel-mode stack (for TSS.RSP0 /
   syscall entry -- see task_t's pml4/kernel_stack_top doc comment).
   Panics on any allocation/mapping failure, or if the image fails ELF64
   validation (would mean the build embedding it is broken, not bad
   runtime input). */
task_t *task_create_user_image(const uint8_t *image_start, const uint8_t *image_end);

/* Thin wrapper: task_create_user_image() with the embedded
   kernel/user/hello.asm image (kernel/sched/user_elf_blob.asm) --
   preserved as its own name since this is what every pre-Milestone-18
   call site (kernel_main's two "hello" processes) already uses, and
   what most of this codebase's own doc comments still refer to by this
   name. */
task_t *task_create_user(void);

/* Milestone 18 (ADR 0018), copy-on-write since Milestone 21 (ADR 0021):
   forks `parent` -- a ring-3 process that is CURRENTLY EXECUTING a
   syscall (this must only ever be called from sys_fork, kernel/arch/
   x86_64/syscall.c, while `parent` is the scheduler's current_task and
   `parent_frame` is that exact syscall's saved GPRs). Builds a brand
   new address space (vmm_create_address_space()) and shares every
   present, process-private page from parent->pml4 into it at the SAME
   virtual address (vmm_for_each_user_page() + vmm_fork_cow_page() per
   page -- the SAME physical frame, refcounted, not a fresh copy), so
   the child's memory reads back identically to the parent's at this
   exact instant even though nothing was actually byte-copied yet -- a
   write from EITHER sibling to a writable shared page lazily triggers
   the real copy (kernel/arch/x86_64/exceptions.c's #PF path,
   vmm_handle_cow_fault()) only if and when it actually happens. The
   child's initial resume context is
   a SYNTHETIC trap_frame_t built from parent_frame's GPRs (same
   register values the parent will resume with) plus parent_user_rsp
   (the parent's user-mode RSP at the moment of this syscall --
   syscall_get_user_rsp(), NOT contained in syscall_frame_t itself) and
   parent_frame->rcx/r11 (SYSCALL's saved user RIP/RFLAGS) -- EXCEPT
   rax, deliberately forced to 0 (fork()'s "this is the child" return
   value; the PARENT's own rax, this task's actual return value, is set
   separately by sys_fork() using the normal syscall-return path).
   Returns the child with a fresh id and parent_id == parent->id, ready
   for the caller (sys_fork) to scheduler_add_task() -- NOT added to the
   ready queue by this function itself, matching every other
   task_create*() function's contract. Panics on any allocation/mapping
   failure -- fork failing wasn't an input-validation case this
   milestone needed to handle gracefully (nothing here can fail from
   bad user input; it only reflects the ALREADY-VALIDATED parent's own
   memory back into a new table). */
task_t *task_fork(task_t *parent, const syscall_frame_t *parent_frame, uint64_t parent_user_rsp);

#endif /* KERNEL_SCHED_TASK_H */
