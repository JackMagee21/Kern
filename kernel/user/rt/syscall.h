#ifndef KERNEL_USER_RT_SYSCALL_H
#define KERNEL_USER_RT_SYSCALL_H

#include <stdint.h>

/* Milestone 24: thin wrappers around this kernel's own syscall ABI
   (kernel/arch/x86_64/syscall.h's SYS_* numbers and calling convention
   -- rdi/rsi for the first two args, rax for the number in/return
   value out; every syscall this kernel has ever needed so far fits in
   two args) for C programs built with the minimal userspace runtime
   (kernel/user/rt/). NOT a POSIX syscall wrapper library -- these
   names/signatures match THIS kernel's own small, custom syscall set
   exactly, nothing more (see Desktop.md for why this distinction is
   deliberate, not incidental). */

uint64_t sys_write(const void *buf, uint64_t len);
_Noreturn void sys_exit(uint64_t code);
uint64_t sys_fork(void);
uint64_t sys_wait(uint64_t target_pid, uint64_t *out_exit_code);
uint64_t sys_exec(uint64_t program_id);
void sys_nop(void);

#endif /* KERNEL_USER_RT_SYSCALL_H */
