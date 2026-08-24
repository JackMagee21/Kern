#include <stdint.h>

#include "task.h"
#include "../arch/x86_64/trap_frame.h"
#include "../arch/x86_64/gdt.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../panic.h"

static uint32_t next_task_id = 1; /* 0 is reserved for the bootstrap task (scheduler.c) */

task_t *task_create(void (*entry)(void))
{
    task_t *task = (task_t *)kmalloc(sizeof(task_t));
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (task == NULL || stack == NULL) {
        panic("task_create: kmalloc failed");
    }

    /* kmalloc's minimum alignment is 16 bytes (libk/heap_alloc.c) and
       TASK_STACK_SIZE is a multiple of 16, so stack_top is already
       16-byte aligned -- the same convention boot.asm's own stack
       relies on before its `call kernel_main`. */
    uint64_t stack_top = (uint64_t)(stack + TASK_STACK_SIZE);

    trap_frame_t *frame = (trap_frame_t *)(stack_top - sizeof(trap_frame_t));
    /* Designated initializer zeroes every field not listed -- all GPRs
       start at 0, which is fine: entry hasn't executed yet, there's no
       real prior state to restore. */
    *frame = (trap_frame_t){
        .rip = (uint64_t)entry,
        .cs = KERNEL_CODE_SELECTOR,
        .rflags = 0x202, /* bit 1: always-1 reserved bit. bit 9: IF=1, task starts with interrupts enabled */
        .rsp = stack_top,
        .ss = KERNEL_DATA_SELECTOR,
    };

    task->rsp = (uint64_t)frame;
    task->kernel_stack_top = stack_top; /* never actually consulted for a ring-0 task; harmless default */
    task->pml4 = vmm_current_pml4(); /* the kernel's own address space, shared by every kernel thread */
    task->next = NULL;
    task->id = next_task_id++;
    return task;
}

/* Private per-process virtual layout: every process uses the SAME
   addresses, which is safe and correct now that each process has its
   own address space -- two processes both "at" the same address are
   backed by independent page tables, so there's no collision to avoid
   the way ADR 0007's shared-address-space design had to.
   Deliberately PML4 index 1 (0x8000000000+), not index 0: PML4[0] is
   reserved entirely for the kernel's shared identity map, which every
   process's address space also carries now (vmm_create_address_space(),
   ADR 0009) -- process-private mappings can't live in the same PML4
   slot as that shared entry. 0x400000/0x600000 within that slot keeps
   the same offsets ADR 0007 originally used (0x400000 is the standard
   ELF load address on x86_64), just relocated to a slot that's actually
   private. */
#define USER_CODE_VIRT_BASE  0x0000008000400000ULL
#define USER_STACK_VIRT_BASE 0x0000008000600000ULL
#define USER_STACK_SIZE      (16u * 1024u)
#define USER_KERNEL_STACK_SIZE (16u * 1024u)

extern char user_demo_start_lma[]; /* boot/linker.ld: physical start/end of kernel/sched/user_demo.asm's blob */
extern char user_demo_end_lma[];

task_t *task_create_user(void)
{
    task_t *task = (task_t *)kmalloc(sizeof(task_t));
    if (task == NULL) {
        panic("task_create_user: kmalloc failed for task_t");
    }

    uint64_t pml4 = vmm_create_address_space();

    /* Map the demo program's code: its own dedicated, page-aligned
       physical page(s) (boot/linker.ld pads .user_demo to full 4KiB
       boundaries specifically so nothing else shares them), read/
       execute, user-accessible, NOT writable. The SAME physical code
       page is mapped into every process created this way -- safe,
       since it's never written to (no VMM_FLAG_WRITABLE), the same way
       real OSes share program text between instances of one program. */
    uint64_t code_phys = (uint64_t)(uintptr_t)user_demo_start_lma;
    uint64_t code_size = (uint64_t)(uintptr_t)user_demo_end_lma - code_phys;
    for (uint64_t off = 0; off < code_size; off += PMM_FRAME_SIZE) {
        if (!vmm_map_page_in(pml4, USER_CODE_VIRT_BASE + off, code_phys + off, VMM_FLAG_USER)) {
            panic("task_create_user: failed to map demo code");
        }
    }

    /* User-accessible stack: fresh physical frames per process (not
       kmalloc -- the kernel heap is supervisor-only mapped; a user
       stack needs its own frames mapped with VMM_FLAG_USER from the
       start). Unlike the code, this must NOT be shared -- each
       process's stack is private. */
    for (uint64_t off = 0; off < USER_STACK_SIZE; off += PMM_FRAME_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            panic("task_create_user: pmm exhausted mapping the user stack");
        }
        if (!vmm_map_page_in(pml4, USER_STACK_VIRT_BASE + off, frame, VMM_FLAG_USER | VMM_FLAG_WRITABLE)) {
            panic("task_create_user: failed to map user stack");
        }
    }
    uint64_t user_stack_top = USER_STACK_VIRT_BASE + USER_STACK_SIZE;

    /* Separate kernel-mode stack: ordinary (supervisor-only) kmalloc'd
       memory (in the KERNEL's own address space, shared/reachable from
       every process's table via the copied PML4[511] entry), used for
       TSS.RSP0 / syscall entry while this task is current. Must NOT be
       user-accessible -- it holds kernel data during interrupt/syscall
       handling. */
    uint8_t *kernel_stack = (uint8_t *)kmalloc(USER_KERNEL_STACK_SIZE);
    if (kernel_stack == NULL) {
        panic("task_create_user: kmalloc failed for the kernel-mode stack");
    }
    uint64_t kernel_stack_top = (uint64_t)(kernel_stack + USER_KERNEL_STACK_SIZE);

    /* The saved/synthetic context lives on the KERNEL stack, not the
       user one -- consistent with where a real preemption would leave
       it (TSS.RSP0 = kernel_stack_top while this task is current, see
       scheduler.c), so "never yet run" and "previously preempted" look
       identical to the scheduler. */
    trap_frame_t *frame = (trap_frame_t *)(kernel_stack_top - sizeof(trap_frame_t));
    *frame = (trap_frame_t){
        .rip = USER_CODE_VIRT_BASE,
        .cs = USER_CODE_SELECTOR | 3,  /* RPL=3 */
        .rflags = 0x202,
        .rsp = user_stack_top,
        .ss = USER_DATA_SELECTOR | 3,  /* RPL=3 */
    };

    task->rsp = (uint64_t)frame;
    task->kernel_stack_top = kernel_stack_top;
    task->pml4 = pml4;
    task->next = NULL;
    task->id = next_task_id++;
    return task;
}
