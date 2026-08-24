; Milestone 7: SYSCALL/SYSRET entry point (IA32_LSTAR target).
;
; On entry: RCX = user return RIP, R11 = user return RFLAGS (both
; clobbered by the SYSCALL instruction itself -- per the standard
; x86_64 syscall ABI, the user program already knows not to expect
; these to survive a syscall, unlike every other GPR). RSP is UNCHANGED
; -- still the user's stack pointer, untrusted, and IF is already
; masked (IA32_FMASK, programmed in syscall.c) so no interrupt can fire
; and use it before we switch away from it below. CS/SS are already the
; kernel selectors (from STAR) and CPL is already 0.
;
; Cannot reuse common_stub.inc: no vector/error_code, no CPU-pushed
; iretq frame, manual (not automatic) stack switch, and the return path
; is sysretq, not iretq. This ALWAYS resumes the exact same context it
; saved (restores RSP from RBX, the pre-align value, not a C return
; value) -- syscall_dispatch itself never switches which task's frame
; gets resumed here; a task switch happening WHILE syscall_dispatch
; runs (Milestone 20, ADR 0020: sys_wait can now block with interrupts
; enabled) is handled entirely by the timer's own separate preemption
; path (common_stub.inc), transparently to this file, exactly like it
; already handles preempting any ordinary kernel thread.
;
; Milestone 20 (ADR 0020): the DURABLE copy of the user RSP now lives
; per-task (task_t::saved_user_rsp, via the syscall_user_rsp_slot
; indirection below) rather than in a single bare global, since a
; SECOND, unrelated task's own complete syscall_entry->exit cycle can
; now genuinely happen while a FIRST task sits blocked inside sys_wait
; (interrupts re-enabled) -- a single shared global would get
; clobbered by the second task's own write before the first ever reads
; it back. `saved_user_rsp` below is now only ever used as TRANSIENT
; scratch, for the brief handoff between "value fetched via the
; per-task indirection" and "value consumed by `mov rsp, ...`" -- both
; windows it's used in run with IF=0 (interrupts still masked: at
; entry, nothing has sti'd yet; at exit, sys_wait's loop always cli's
; again before returning -- see syscall.c's sys_wait), so no OTHER
; task's syscall can interleave and clobber it mid-handoff. It must
; NEVER be read across a window where IF could be 1.
default abs

section .bss
align 8
saved_user_rsp: resq 1

section .text
bits 64

extern syscall_dispatch
extern syscall_kernel_rsp
extern syscall_user_rsp_slot

global syscall_entry
syscall_entry:
    mov [saved_user_rsp], rsp ; transient: IF still masked here (SFMASK), safe
    mov rsp, [syscall_kernel_rsp]

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Record this task's user RSP into ITS OWN durable per-task slot,
    ; now that rax/rcx are free scratch -- their real values are
    ; already safely pushed above, restored by the `pop` sequence below.
    mov rax, [syscall_user_rsp_slot] ; rax = &current_task->saved_user_rsp
    mov rcx, [saved_user_rsp]
    mov [rax], rcx

    mov rbx, rsp          ; true frame pointer, preserved across the call (callee-saved)
    mov rdi, rsp          ; arg 1: syscall_frame_t *
    and rsp, ~0xf         ; SysV ABI: RSP must be 16-byte aligned at `call`
    call syscall_dispatch
    mov rsp, rbx          ; restore

    ; Re-fetch via the per-task indirection (NOT the transient scratch
    ; above, which may have been overwritten by another task's own
    ; syscall while this one was blocked inside the call above) --
    ; syscall_user_rsp_slot is guaranteed to point at OUR OWN slot
    ; again here, since we're the task currently executing. rax/rbx are
    ; free scratch again: their real values are restored by `pop`
    ; below, AFTER this.
    mov rax, [syscall_user_rsp_slot]
    mov rax, [rax]
    mov [saved_user_rsp], rax  ; transient: IF is masked again here -- see sys_wait

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    mov rsp, [saved_user_rsp]
    o64 sysret
