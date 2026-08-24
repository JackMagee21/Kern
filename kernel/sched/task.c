#include <stdint.h>

#include "task.h"
#include "../arch/x86_64/trap_frame.h"
#include "../arch/x86_64/gdt.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/elf_loader.h"
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
    task->parent_id = 0; /* kernel threads never exit and are never forked -- harmless default */
    task->exit_code = 0;
    return task;
}

/* Private per-process virtual layout: every process uses the SAME
   stack address, which is safe and correct now that each process has
   its own address space -- two processes both "at" the same address
   are backed by independent page tables, so there's no collision to
   avoid the way ADR 0007's shared-address-space design had to. The
   code's own virtual address now comes from the loaded ELF image's own
   program headers (kernel/user/user.ld links it at 0x8000400000, the
   same slot ADR 0007/0009 originally hardcoded) rather than being
   hardcoded here -- see elf_load(), ADR 0017. */
#define USER_STACK_VIRT_BASE 0x0000008000600000ULL
#define USER_STACK_SIZE      (16u * 1024u)
#define USER_KERNEL_STACK_SIZE (16u * 1024u)

extern const uint8_t user_elf_image_start[]; /* kernel/sched/user_elf_blob.asm: embedded build/kernel/user/hello.elf */
extern const uint8_t user_elf_image_end[];

task_t *task_create_user_image(const uint8_t *image_start, const uint8_t *image_end)
{
    task_t *task = (task_t *)kmalloc(sizeof(task_t));
    if (task == NULL) {
        panic("task_create_user_image: kmalloc failed for task_t");
    }

    uint64_t pml4 = vmm_create_address_space();

    /* Milestone 17 (ADR 0017): parse and map a real compiled ELF64
       executable instead of Milestone 7-16's single hand-mapped,
       shared-read-only demo code page. Every PT_LOAD segment gets its
       OWN freshly allocated frame(s) in THIS process's address space
       (VMM_FLAG_OWNED, so exit correctly frees them -- ADR 0010), with
       per-segment W^X permissions derived from the ELF's own p_flags
       rather than one fixed policy for the whole program. elf_load()
       only returns false for a malformed image -- every image this
       repo embeds is built by its own Makefile/user.ld, so a
       validation failure here would mean the build itself is broken,
       not bad runtime input; panic rather than leave a half-built
       process behind. */
    uint64_t image_size = (uint64_t)(uintptr_t)image_end - (uint64_t)(uintptr_t)image_start;
    uint64_t entry_point;
    if (!elf_load(pml4, image_start, image_size, &entry_point)) {
        panic("task_create_user_image: embedded user ELF image failed validation");
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
        .rip = entry_point,
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
    task->parent_id = 0; /* spawned directly by kernel_main -- orphan, nothing will ever wait() for it */
    task->exit_code = 0;
    return task;
}

task_t *task_create_user(void)
{
    return task_create_user_image(user_elf_image_start, user_elf_image_end);
}

/* Milestone 18 (ADR 0018): forking a process's address space. Runs
   under the PARENT's own CR3 (task_fork() is only ever called from
   sys_fork, itself only ever reached mid-syscall -- SYSCALL never
   switches CR3, so the parent's mappings are directly readable via
   ordinary virtual addresses right now, no special access trick
   needed for the SOURCE side, unlike the ELF loader's need to write a
   brand new, not-yet-mapped-anywhere DESTINATION frame directly via
   its physical address). */
typedef struct {
    uint64_t dest_pml4;
} fork_copy_ctx_t;

static void fork_copy_page(uint64_t va, uint64_t phys, uint64_t flags, void *ctx_)
{
    (void)phys; /* the source frame's physical address is never dereferenced -- see comment above: read via va instead */
    fork_copy_ctx_t *ctx = (fork_copy_ctx_t *)ctx_;

    uint64_t new_frame = pmm_alloc_frame();
    if (new_frame == 0 || new_frame >= VMM_IDENTITY_WINDOW_LIMIT) {
        panic("task_fork: pmm exhausted or destination frame outside identity window");
    }

    uint8_t *dst = (uint8_t *)(uintptr_t)new_frame;
    const uint8_t *src = (const uint8_t *)(uintptr_t)va;
    for (uint64_t b = 0; b < PMM_FRAME_SIZE; b++) {
        dst[b] = src[b];
    }

    if (!vmm_map_page_in(ctx->dest_pml4, va, new_frame, flags)) {
        panic("task_fork: vmm_map_page_in failed while copying the parent's address space");
    }
}

task_t *task_fork(task_t *parent, const syscall_frame_t *parent_frame, uint64_t parent_user_rsp)
{
    task_t *child = (task_t *)kmalloc(sizeof(task_t));
    if (child == NULL) {
        panic("task_fork: kmalloc failed for task_t");
    }

    uint64_t child_pml4 = vmm_create_address_space();

    fork_copy_ctx_t ctx = { .dest_pml4 = child_pml4 };
    vmm_for_each_user_page(parent->pml4, fork_copy_page, &ctx);

    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top = alloc_kernel_stack(USER_KERNEL_STACK_SIZE, &kernel_stack_base);

    /* Every GPR copied from the parent's saved syscall state EXCEPT
       rax, forced to 0 -- the one field that must NOT match the
       parent's (fork()'s "you are the child" signal). rip/rflags come
       from rcx/r11, exactly as a normal SYSRET would reconstruct them
       (see syscall_frame_t's doc comment) -- this is what lets the
       child resume at the SAME point in the program right after the
       `syscall` instruction that forked it, just like the parent will. */
    trap_frame_t *frame = (trap_frame_t *)(kernel_stack_top - sizeof(trap_frame_t));
    *frame = (trap_frame_t){
        .r15 = parent_frame->r15, .r14 = parent_frame->r14, .r13 = parent_frame->r13, .r12 = parent_frame->r12,
        .r11 = parent_frame->r11, .r10 = parent_frame->r10, .r9 = parent_frame->r9, .r8 = parent_frame->r8,
        .rbp = parent_frame->rbp, .rdi = parent_frame->rdi, .rsi = parent_frame->rsi, .rdx = parent_frame->rdx,
        .rcx = parent_frame->rcx, .rbx = parent_frame->rbx,
        .rax = 0,
        .rip = parent_frame->rcx,
        .cs = USER_CODE_SELECTOR | 3,
        .rflags = parent_frame->r11,
        .rsp = parent_user_rsp,
        .ss = USER_DATA_SELECTOR | 3,
    };

    child->rsp = (uint64_t)frame;
    child->kernel_stack_top = kernel_stack_top;
    child->kernel_stack_base = kernel_stack_base;
    child->pml4 = child_pml4;
    child->state = TASK_READY;
    child->next = NULL;
    child->prev = NULL;
    child->id = next_task_id++;
    child->parent_id = parent->id;
    child->exit_code = 0;
    return child;
}
