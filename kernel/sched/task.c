#include <stdint.h>

#include "task.h"
#include "../arch/x86_64/trap_frame.h"
#include "../arch/x86_64/gdt.h"
#include "../mm/heap.h"
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
    task->next = NULL;
    task->id = next_task_id++;
    return task;
}
