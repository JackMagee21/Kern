#include <stdint.h>

#include "syscall.h"
#include "gdt.h"
#include "msr.h"
#include "../../drivers/console.h"
#include "../../mm/vmm.h"
#include "../../sched/scheduler.h"
#include "../../sched/task.h"

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
extern uint64_t saved_user_rsp;  /* syscall_entry.asm -- Milestone 18, see syscall_get_user_rsp() */

/* Not static: syscall_entry.asm references this symbol directly
   (`extern syscall_kernel_rsp`) to find the stack to switch to. */
uint64_t syscall_kernel_rsp;

static uint64_t syscall_count;

uint64_t syscall_get_user_rsp(void)
{
    return saved_user_rsp;
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
        console_putc((char)buf[i]);
    }
    frame->rax = len;
}

/* Milestone 18 (ADR 0018): forks the CALLING process (task_fork() does
   the actual work -- new address space, deep-copied pages, a synthetic
   child trap frame built from THIS frame plus the user RSP at the
   moment of this exact syscall). Returns the child's task id to the
   PARENT (this frame's rax, the normal syscall-return-value path); the
   child's own rax is baked into its synthetic frame as 0 by
   task_fork() and is never touched here -- the child doesn't resume
   through syscall_dispatch/sysretq at all, it resumes fresh via the
   scheduler's normal iretq path, same as any newly created task. */
static void sys_fork(syscall_frame_t *frame)
{
    task_t *parent = scheduler_current_task();
    task_t *child = task_fork(parent, frame, syscall_get_user_rsp());
    scheduler_add_task(child);
    frame->rax = child->id;
}

/* Milestone 18 (ADR 0018): NON-blocking. Syscalls in this kernel are
   non-preemptible and run with interrupts masked the whole time
   (SFMASK, syscall.c's syscall_init()) -- a genuinely BLOCKING wait()
   would need to sleep with interrupts enabled and a way for something
   else to wake this task back up, neither of which this kernel has yet
   (no blocking/sleep-queue primitive exists at all -- flagged future
   work, ADR 0018's Known Limitations). Instead: rdi = target pid (0 =
   any child of the caller), rsi = optional user pointer to write the
   exit code into (0 = caller doesn't want it, skip the write). Returns
   the reaped child's pid in rax, or 0 if no exited-but-uncollected
   child matches yet -- the caller is expected to poll (see
   kernel/user/fork_demo.asm's wait loop), the same "hlt/spin until a
   counter advances" pattern this kernel already uses everywhere else
   for "wait for an async event", just done from ring 3 via repeated
   syscalls instead of from kernel_main directly. */
static void sys_wait(syscall_frame_t *frame)
{
    uint64_t target_pid = frame->rdi;
    uint64_t out_ptr = frame->rsi;

    uint64_t exit_code = 0;
    uint32_t reaped_pid = scheduler_try_wait(scheduler_current_task()->id, (uint32_t)target_pid, &exit_code);

    if (reaped_pid != 0 && out_ptr != 0) {
        if (!vmm_is_user_range(out_ptr, sizeof(uint64_t))) {
            frame->rax = (uint64_t)-1;
            return;
        }
        *(uint64_t *)(uintptr_t)out_ptr = exit_code;
    }

    frame->rax = reaped_pid;
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
    case SYS_EXIT:
        scheduler_exit_current(frame->rdi); /* noreturn -- never falls through to sysretq; rdi = exit code */
        break;
    case SYS_FORK:
        sys_fork(frame);
        break;
    case SYS_WAIT:
        sys_wait(frame);
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
