#ifndef KERNEL_ARCH_X86_64_TSS_H
#define KERNEL_ARCH_X86_64_TSS_H

#include <stdint.h>

/* Allocates a default kernel stack, builds the TSS, and loads it via
   ltr. Must run after gdt_init() has installed the TSS descriptor. */
void tss_init(void);

/* Updates RSP0 -- the stack the CPU switches to on any interrupt/
   exception that raises privilege from ring 3 to ring 0. Called by the
   scheduler on every task switch: each ring-3 task has its OWN
   dedicated kernel stack (see kernel/sched/task.c), because the CPU
   always starts from the SAME fixed RSP0 address on every such
   transition -- if two different ring-3 tasks' preempted contexts both
   landed on one shared stack, the second one to be interrupted would
   silently overwrite the first's still-pending saved state. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* KERNEL_ARCH_X86_64_TSS_H */
