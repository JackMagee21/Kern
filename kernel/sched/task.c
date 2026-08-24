#include <stdint.h>

#include "task.h"
#include "../arch/x86_64/trap_frame.h"
#include "../arch/x86_64/gdt.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../panic.h"

static uint32_t next_task_id = 1; /* 0 is reserved for the bootstrap task (scheduler.c) */

/* Every task's kernel-mode stack (a kernel thread's whole stack via
   task_create(), or a ring-3 process's separate kernel-mode stack via
   task_create_user()) gets its own dedicated, page-mapped VA slot in
   this region, preceded by one deliberately-unmapped guard page -- a
   stack overflow growing downward now hits that unmapped page and
   takes an immediate #PF instead of silently corrupting whatever
   kmalloc'd heap object happened to sit right below it (kernel stacks
   used to come straight from kmalloc(), packed with no gap at all
   between allocations -- a real, if latent, risk this closes off).
   PML4[511]:PDPT[509] -- distinct from the kernel image (PDPT[510],
   boot.asm) and the kernel heap (PDPT[511], heap.c); verified via
   python3, the same discipline as every other PML4/PDPT index in this
   codebase (see ADR 0012).

   VA slots are handed out by a simple monotonic bump allocator and are
   NEVER reclaimed/reused even after a task is reaped -- the same
   "simple now, revisit only if actually exhausted" scope boundary
   heap_init()'s fixed-size region already accepted (ADR 0004); a 1GiB
   region divided into ~20KiB slots is nowhere near exhausted by
   anything this kernel creates. */
#define KERNEL_STACK_REGION_VIRT_BASE 0xFFFFFFFF40000000ULL
#define KERNEL_STACK_GUARD_SIZE       PMM_FRAME_SIZE

static uint64_t next_kernel_stack_slot = KERNEL_STACK_REGION_VIRT_BASE;

/* Allocates `size` (a multiple of PMM_FRAME_SIZE) bytes of kernel-mode
   stack, mapped VMM_FLAG_WRITABLE | VMM_FLAG_NX (data, never code --
   same W^X reasoning as the kernel heap, ADR 0011). Returns the
   stack's top (matches task_t::kernel_stack_top) and sets *out_base to
   its bottom (matches task_t::kernel_stack_base). Panics on any
   allocation/mapping failure. */
static uint64_t alloc_kernel_stack(uint64_t size, uint64_t *out_base)
{
    uint64_t base_va = next_kernel_stack_slot + KERNEL_STACK_GUARD_SIZE;
    next_kernel_stack_slot = base_va + size;

    for (uint64_t off = 0; off < size; off += PMM_FRAME_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            panic("alloc_kernel_stack: pmm exhausted");
        }
        if (!vmm_map_page(base_va + off, frame, VMM_FLAG_WRITABLE | VMM_FLAG_NX)) {
            panic("alloc_kernel_stack: vmm_map_page failed");
        }
    }

    *out_base = base_va;
    return base_va + size;
}

void task_free_kernel_stack(uint64_t base, uint64_t size)
{
    for (uint64_t off = 0; off < size; off += PMM_FRAME_SIZE) {
        uint64_t phys;
        if (vmm_translate(base + off, &phys)) {
            vmm_unmap_page(base + off);
            pmm_free_frame(phys);
        }
    }
}

task_t *task_create(void (*entry)(void))
{
    task_t *task = (task_t *)kmalloc(sizeof(task_t));
    if (task == NULL) {
        panic("task_create: kmalloc failed");
    }

    uint64_t stack_base;
    uint64_t stack_top = alloc_kernel_stack(TASK_STACK_SIZE, &stack_base);

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
    task->kernel_stack_base = stack_base;
    task->pml4 = vmm_current_pml4(); /* the kernel's own address space, shared by every kernel thread */
    task->state = TASK_READY;
    task->next = NULL;
    task->prev = NULL;
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
       real OSes share program text between instances of one program.
       Deliberately no VMM_FLAG_OWNED either (contrast the stack mapping
       below): this frame is part of the kernel image itself, never
       pmm_alloc_frame()'d, so a process exiting must never
       pmm_free_frame() it back -- ADR 0010. */
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
       process's stack is private. VMM_FLAG_OWNED marks these frames as
       this process's own, so vmm_destroy_address_space() (ADR 0010)
       actually frees them back to the pmm on exit -- unlike the code
       page above, which deliberately omits it. VMM_FLAG_NX (W^X):
       nothing legitimate ever executes from a stack, and a writable
       page a ring-3 program can fill with arbitrary bytes and then
       jump into is exactly the classic stack-smashing code-injection
       primitive -- marking it non-executable closes that off at the
       page-table level regardless of what any particular program does
       or doesn't check. Nothing is deliberately mapped just below
       USER_STACK_VIRT_BASE either, so a downward overflow already
       lands on an unmapped page and faults, the same guard-page
       protection the dedicated kernel-stack region below gives kernel-
       mode stacks explicitly (ADR 0012). */
    for (uint64_t off = 0; off < USER_STACK_SIZE; off += PMM_FRAME_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            panic("task_create_user: pmm exhausted mapping the user stack");
        }
        if (!vmm_map_page_in(pml4, USER_STACK_VIRT_BASE + off, frame, VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_OWNED | VMM_FLAG_NX)) {
            panic("task_create_user: failed to map user stack");
        }
    }
    uint64_t user_stack_top = USER_STACK_VIRT_BASE + USER_STACK_SIZE;

    /* Separate kernel-mode stack: its own dedicated, guard-paged VA
       slot (alloc_kernel_stack(), same as a kernel thread's stack
       above) rather than kmalloc(), in the KERNEL's own address space
       (shared/reachable from every process's table via the copied
       PML4[511] entry), used for TSS.RSP0 / syscall entry while this
       task is current. Must NOT be user-accessible -- it holds kernel
       data during interrupt/syscall handling; alloc_kernel_stack()
       never sets VMM_FLAG_USER, so it isn't. */
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top = alloc_kernel_stack(USER_KERNEL_STACK_SIZE, &kernel_stack_base);

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
    task->kernel_stack_base = kernel_stack_base;
    task->pml4 = pml4;
    task->state = TASK_READY;
    task->next = NULL;
    task->prev = NULL;
    task->id = next_task_id++;
    return task;
}
