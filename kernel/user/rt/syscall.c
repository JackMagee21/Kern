#include <stdint.h>

#include "syscall.h"

/* Milestone 24: the one place the raw `syscall` instruction is issued
   from userspace C code. Clobbers exactly RCX/R11 (the SYSCALL
   instruction's own hardware-mandated clobbers, per the x86-64 SysV
   syscall ABI -- confirmed, not guessed, by this exact codebase's own
   kernel/arch/x86_64/syscall_entry.asm: every OTHER GPR is saved and
   restored around syscall_dispatch(), so from a caller's perspective
   nothing else changes) plus "memory" (a syscall can have side effects
   on memory this compilation unit can't see -- sys_write reads the
   caller's own buffer, sys_wait writes through out_exit_code). */
static inline uint64_t syscall2(uint64_t num, uint64_t a1, uint64_t a2)
{
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

uint64_t sys_write(const void *buf, uint64_t len)
{
    return syscall2(1, (uint64_t)(uintptr_t)buf, len);
}

_Noreturn void sys_exit(uint64_t code)
{
    syscall2(2, code, 0);
    for (;;) {
    } /* unreachable if sys_exit actually works; defensive only, same
         stance as every hand-written .asm demo's own trailing .hang loop */
}

uint64_t sys_fork(void)
{
    return syscall2(3, 0, 0);
}

uint64_t sys_wait(uint64_t target_pid, uint64_t *out_exit_code)
{
    return syscall2(4, target_pid, (uint64_t)(uintptr_t)out_exit_code);
}

uint64_t sys_exec(uint64_t program_id)
{
    return syscall2(5, program_id, 0);
}

void sys_nop(void)
{
    syscall2(0, 0, 0);
}
