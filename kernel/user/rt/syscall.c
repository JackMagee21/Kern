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

uint64_t sys_ipc_send(uint64_t dest_pid, const ipc_message_t *msg)
{
    return syscall2(6, dest_pid, (uint64_t)(uintptr_t)msg);
}

void sys_ipc_recv(ipc_message_t *out)
{
    syscall2(7, (uint64_t)(uintptr_t)out, 0);
}

uint64_t sys_shm_create(uint64_t size)
{
    return syscall2(8, size, 0);
}

uint64_t sys_shm_map(uint64_t shm_id)
{
    return syscall2(9, shm_id, 0);
}

/* Milestone 27 (ADR 0027): sys_fb_present needs 5 real arguments (x, y,
   w, h, buf) -- the first syscall this runtime has ever needed more
   than two for. Uses GCC's register-variable idiom (the standard way
   to pin a specific value into a specific register for inline asm,
   the same technique Linux's own raw syscall() wrappers use) to place
   a4/a5 into r10/r8 -- the exact 3rd/4th/5th-argument registers this
   kernel's own syscall ABI convention already documents (kernel/arch/
   x86_64/syscall.h's own top-of-file comment: RDI/RSI/RDX/R10/R8/R9),
   with a1/a2/a3 going into RDI/RSI/RDX via the ordinary "D"/"S"/"d"
   constraints syscall2() above already uses for the first two. */
static inline uint64_t syscall5(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

uint64_t sys_fb_acquire(void)
{
    return syscall2(10, 0, 0);
}

uint64_t sys_fb_present(uint64_t x, uint64_t y, uint64_t w, uint64_t h, const void *buf)
{
    return syscall5(11, x, y, w, h, (uint64_t)(uintptr_t)buf);
}

uint64_t sys_input_subscribe(void)
{
    return syscall2(12, 0, 0);
}
