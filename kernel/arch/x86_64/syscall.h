#ifndef KERNEL_ARCH_X86_64_SYSCALL_H
#define KERNEL_ARCH_X86_64_SYSCALL_H

#include <stdint.h>

/* Standard x86_64 SYSCALL ABI convention (same one Linux uses, adopted
   deliberately rather than inventing a new one): RDI/RSI/RDX/R10/R8/R9
   for args 1-6 (R10, not RCX, for arg 4 -- RCX is clobbered by the
   SYSCALL instruction itself), RAX for the syscall number in / return
   value out. Field order matches trap_frame_t's GPR portion for
   familiarity, but this is NOT a trap_frame_t: no vector/error_code/
   cs/ss/rip/rflags/rsp, since SYSCALL doesn't push any of those --
   RCX/R11 (the user's return RIP/RFLAGS) just ride along in their
   normal GPR slots and are naturally still correct when
   syscall_entry.asm's plain pop sequence restores them before sysretq,
   since nothing in between touches them except a syscall handler that
   deliberately wants to (none do). */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} syscall_frame_t;

#define SYS_NOP   0
#define SYS_WRITE 1

/* Programs STAR/LSTAR/SFMASK and sets EFER.SCE. Must run after
   gdt_init() (STAR encodes GDT selector offsets) and tss_init().
   syscall_set_kernel_stack() must be called at least once before any
   ring-3 code executes -- there's no default kernel stack otherwise. */
void syscall_init(void);

/* Updates the kernel stack syscall_entry.asm switches to on entry.
   Called by the scheduler on every task switch, same as
   tss_set_rsp0() -- see kernel/sched/scheduler.c and ADR 0007 for why
   each ring-3 task needs its own dedicated kernel stack rather than a
   shared one. */
void syscall_set_kernel_stack(uint64_t top);

/* Defined in irq_dispatch.c-style dispatch (here: syscall.c). Called
   from syscall_entry.asm; frame->rax selects the syscall, frame->rax
   is overwritten with the return value. */
void syscall_dispatch(syscall_frame_t *frame);

/* Total syscalls serviced so far -- lets kernel_main's self-test poll
   for evidence the ring-3 demo task's syscalls are actually landing,
   the same pattern as pit.c's tick_count / kernel/sched's demo task
   counters. */
uint64_t syscall_get_count(void);

#endif /* KERNEL_ARCH_X86_64_SYSCALL_H */
