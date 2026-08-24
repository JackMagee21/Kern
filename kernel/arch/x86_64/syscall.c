#include <stdint.h>

#include "syscall.h"
#include "gdt.h"
#include "../../drivers/serial.h"
#include "../../mm/vmm.h"

/*
 * MSR numbers and the STAR encoding verified against Linux's own
 * arch/x86/include/asm/msr-index.h and syscall_init()
 * (arch/x86/kernel/cpu/common.c: STAR = (__USER32_CS << 16) |
 * __KERNEL_CS, high 32 bits) -- not derived from memory alone, since
 * the GDT ordering SYSRET depends on (gdt.h) is a well-known,
 * easy-to-get-wrong gotcha. EFER.SCE verified against Linux's
 * msr-index.h EFER bit definitions.
 */
#define MSR_STAR    0xC0000081u
#define MSR_LSTAR   0xC0000082u
#define MSR_SFMASK  0xC0000084u
#define MSR_EFER    0xC0000080u
#define EFER_SCE    (1ULL << 0)

/* Masked out of RFLAGS by the CPU on SYSCALL entry, before
   syscall_entry.asm's first instruction runs: IF (bit 9), so no
   interrupt can fire while RSP still holds the untrusted user stack
   pointer (the window between SYSCALL and this file's manual stack
   switch) -- CLAUDE.md's "never dereference user-supplied
   pointers/lengths" spirit extended to "never let an async event use
   one either, even transiently". TF (bit 8) masked too, defensively --
   no reason a stray single-step trap should fire during kernel entry. */
#define SFMASK_VALUE 0x300u

extern void syscall_entry(void); /* syscall_entry.asm */

/* Not static: syscall_entry.asm references this symbol directly
   (`extern syscall_kernel_rsp`) to find the stack to switch to. */
uint64_t syscall_kernel_rsp;

static uint64_t syscall_count;

static void write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xffffffffu);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void syscall_init(void)
{
    uint64_t star = ((uint64_t)USER32_CS_PLACEHOLDER << 48) | ((uint64_t)KERNEL_CODE_SELECTOR << 32);
    write_msr(MSR_STAR, star);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    write_msr(MSR_SFMASK, SFMASK_VALUE);

    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | EFER_SCE);
}

void syscall_set_kernel_stack(uint64_t top)
{
    syscall_kernel_rsp = top;
}

static void sys_write(syscall_frame_t *frame)
{
    uint64_t ptr = frame->rdi;
    uint64_t len = frame->rsi;

    if (!vmm_is_user_range(ptr, len)) {
        frame->rax = (uint64_t)-1;
        return;
    }

    const uint8_t *buf = (const uint8_t *)(uintptr_t)ptr;
    for (uint64_t i = 0; i < len; i++) {
        serial_putc((char)buf[i]);
    }
    frame->rax = len;
}

void syscall_dispatch(syscall_frame_t *frame)
{
    syscall_count++;

    switch (frame->rax) {
    case SYS_NOP:
        frame->rax = 0;
        break;
    case SYS_WRITE:
        sys_write(frame);
        break;
    default:
        frame->rax = (uint64_t)-1;
        break;
    }
}

uint64_t syscall_get_count(void)
{
    return syscall_count;
}
