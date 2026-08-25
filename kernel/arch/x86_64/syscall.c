#include <stddef.h>
#include <stdint.h>

#include "syscall.h"
#include "gdt.h"
#include "msr.h"
#include "../../drivers/console.h"
#include "../../mm/vmm.h"
#include "../../sched/scheduler.h"
#include "../../sched/task.h"
#include "../../ipc/msgqueue.h"
#include "../../ipc/shm.h"

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

/* Milestone 20 (ADR 0020): not static -- syscall_entry.asm dereferences
   this directly (`extern syscall_user_rsp_slot`) to find WHERE to save/
   restore the current task's own user-mode RSP. Always points at the
   currently-scheduled task's task_t::saved_user_rsp field (see
   syscall_set_user_rsp_slot()'s doc comment, syscall.h). */
uint64_t *syscall_user_rsp_slot;

static uint64_t syscall_count;

/* Milestone 20 (ADR 0020): incremented once per sti/hlt/cli cycle
   sys_wait actually takes -- i.e. once per turn it found nothing yet
   and genuinely blocked, as opposed to succeeding on its very first
   check. Exists purely so kernel_main's self-test can prove sys_wait
   REALLY blocked at least once (the fork/wait demo's parent calls
   sys_wait immediately after sys_fork returns, almost certainly before
   the freshly created child has even had its first turn -- but "the
   right exit code came back" alone doesn't distinguish a genuine block
   from a lucky immediate success, the same observability gap
   syscall_get_count()/scheduler_reaped_count() already exist to close
   for their own milestones). */
static uint64_t sys_wait_block_count;

/* Milestone 22 (ADR 0022): incremented once per successful task_exec()
   -- see syscall.h's doc comment on syscall_get_exec_count(). */
static uint64_t sys_exec_count;

uint64_t syscall_get_user_rsp(void)
{
    return *syscall_user_rsp_slot;
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

void syscall_set_user_rsp_slot(uint64_t *slot)
{
    syscall_user_rsp_slot = slot;
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

/* Milestone 20 (ADR 0020): genuinely BLOCKING. rdi = target pid (0 =
   any child of the caller), rsi = optional user pointer to write the
   exit code into (0 = caller doesn't want it, skip the write). Blocks
   -- sti/hlt/cli, re-polling scheduler_try_wait() once per own turn --
   until a matching exited-but-uncollected child appears, then returns
   its pid in rax. Deliberately does NOT introduce a new TASK_BLOCKED
   state or a wake-list: this task stays TASK_READY in the ordinary
   ready queue the entire time, simply doing nothing useful on most of
   its turns, and the ALREADY-EXISTING preemptive round-robin scheduler
   (Milestone 6) is what actually gives every other task (including the
   reaper, which is what produces a match) a chance to run in between.
   Safe to re-enable interrupts here specifically because
   syscall_get_user_rsp()'s storage was moved to be per-task this same
   milestone -- see ADR 0020 for why the OLD single-global storage
   would have been corrupted by another task's own syscall landing
   while this one sits blocked. A caller with NO children at all (never
   forked) blocks here forever -- an accepted known limitation, the
   same missing "live child list" gap ADR 0018 already flagged, now
   surfacing as an infinite block instead of an infinite userspace poll
   loop. */
static void sys_wait(syscall_frame_t *frame)
{
    uint64_t target_pid = frame->rdi;
    uint64_t out_ptr = frame->rsi;
    uint32_t caller_id = scheduler_current_task()->id;

    uint64_t exit_code = 0;
    uint32_t reaped_pid;
    for (;;) {
        reaped_pid = scheduler_try_wait(caller_id, (uint32_t)target_pid, &exit_code);
        if (reaped_pid != 0) {
            break;
        }
        sys_wait_block_count++;
        __asm__ volatile("sti; hlt; cli");
    }

    if (out_ptr != 0) {
        if (!vmm_is_user_range(out_ptr, sizeof(uint64_t))) {
            frame->rax = (uint64_t)-1;
            return;
        }
        *(uint64_t *)(uintptr_t)out_ptr = exit_code;
    }

    frame->rax = reaped_pid;
}

/* Milestone 22 (ADR 0022): rdi = program_id, an index into task.c's own
   small fixed table of embedded images (no filesystem exists to load an
   arbitrary path from). On success, task_exec() has already overwritten
   *frame in place with the new image's entry context (rcx/r11) and
   zeroed every other GPR -- this function's ONLY remaining job is the
   count and letting syscall_dispatch fall through to the ordinary
   sysretq epilogue, which now resumes into the DIFFERENT program rather
   than the one that made this syscall. On failure (bad program_id),
   *frame is untouched by task_exec() -- just report -1 in rax, same as
   any other validated-input syscall failure, and the OLD image resumes
   normally. */
static void sys_exec(syscall_frame_t *frame)
{
    task_t *current = scheduler_current_task();
    uint32_t program_id = (uint32_t)frame->rdi;

    if (!task_exec(current, frame, program_id)) {
        frame->rax = (uint64_t)-1;
        return;
    }

    sys_exec_count++;
}

/* Milestone 26 (ADR 0026): incremented once per turn sys_ipc_recv's
   blocking loop actually calls scheduler_block_current() without a
   message already waiting -- see syscall.h's doc comment on
   syscall_get_ipc_recv_block_count(). */
static uint64_t sys_ipc_recv_block_count;

/* rdi = destination pid, rsi = pointer to an ipc_message_t-shaped
   struct in the caller's OWN user memory (validated before copying it
   INTO the kernel, same "validate then copy by value" discipline
   sys_write already established). sender_pid is overwritten with the
   ACTUAL caller's own id here, in the kernel -- never trusted from
   whatever the caller's own struct happened to contain, the same
   "don't trust a user-supplied identity claim" stance this codebase
   already applies elsewhere (e.g. sys_wait never trusts a caller-
   supplied parent_id, it reads the real one off task_t). Fails (-1)
   if msg_ptr isn't a validated user pointer, dest_pid names no live
   task (scheduler_find_task()), or dest's inbox is currently full
   (ipc_send()'s own documented drop-on-full contract). */
static void sys_ipc_send(syscall_frame_t *frame)
{
    uint64_t dest_pid = frame->rdi;
    uint64_t msg_ptr = frame->rsi;

    if (!vmm_is_user_range(msg_ptr, sizeof(ipc_message_t))) {
        frame->rax = (uint64_t)-1;
        return;
    }

    task_t *dest = scheduler_find_task((uint32_t)dest_pid);
    if (dest == NULL) {
        frame->rax = (uint64_t)-1;
        return;
    }

    ipc_message_t msg = *(const ipc_message_t *)(uintptr_t)msg_ptr;
    msg.sender_pid = scheduler_current_task()->id;

    if (!ipc_send(dest, &msg)) {
        frame->rax = (uint64_t)-1;
        return;
    }
    frame->rax = 0;
}

/* rdi = pointer to write the received ipc_message_t into (the caller's
   OWN user memory, validated up front -- the message itself is only
   ever written there once one has actually arrived, so there's no
   partial-write-then-fail case to worry about). ALWAYS blocks until a
   message arrives -- no non-blocking variant is exposed yet, since
   nothing this milestone needs one (a real event loop always wants to
   block here; YAGNI on a poll-style variant until something actually
   needs it). This is scheduler_block_current()'s first REAL consumer
   outside its own Milestone 25 self-test. */
static void sys_ipc_recv(syscall_frame_t *frame)
{
    uint64_t out_ptr = frame->rdi;
    if (!vmm_is_user_range(out_ptr, sizeof(ipc_message_t))) {
        frame->rax = (uint64_t)-1;
        return;
    }

    task_t *self = scheduler_current_task();
    ipc_message_t msg;
    while (!ipc_try_recv(self, &msg)) {
        sys_ipc_recv_block_count++;
        scheduler_block_current();
    }

    *(ipc_message_t *)(uintptr_t)out_ptr = msg;
    frame->rax = 0;
}

/* rdi = requested size in bytes. Returns a new shm object id in rax, or
   0 on failure (bad size, or the object table is full -- see
   shm_create()'s own doc comment, kernel/ipc/shm.h). The CALLER still
   has to call sys_shm_map() itself afterward to actually get a usable
   pointer -- creating does not implicitly map, see shm.h for why. */
static void sys_shm_create(syscall_frame_t *frame)
{
    uint64_t size = frame->rdi;
    frame->rax = shm_create(size);
}

/* rdi = an shm object id (from sys_shm_create(), or learned from
   another process via a real IPC message -- e.g. Desktop.md's own
   handoff self-test). Returns the mapped virtual address in the
   CALLER's own address space in rax, or 0 on failure (unknown id, or
   this process's own shm VA budget exhausted). */
static void sys_shm_map(syscall_frame_t *frame)
{
    uint32_t shm_id = (uint32_t)frame->rdi;
    task_t *current = scheduler_current_task();
    frame->rax = shm_map(shm_id, current, NULL);
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
    case SYS_EXEC:
        sys_exec(frame);
        break;
    case SYS_IPC_SEND:
        sys_ipc_send(frame);
        break;
    case SYS_IPC_RECV:
        sys_ipc_recv(frame);
        break;
    case SYS_SHM_CREATE:
        sys_shm_create(frame);
        break;
    case SYS_SHM_MAP:
        sys_shm_map(frame);
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

uint64_t syscall_get_wait_block_count(void)
{
    return sys_wait_block_count;
}

uint64_t syscall_get_exec_count(void)
{
    return sys_exec_count;
}

uint64_t syscall_get_ipc_recv_block_count(void)
{
    return sys_ipc_recv_block_count;
}
