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
; is sysretq, not iretq. Syscalls are deliberately non-preemptible and
; never switch tasks in this milestone (ADR 0007) -- unlike
; common_stub.inc, this always resumes the exact same context it saved,
; so it restores RSP from RBX (the pre-align value), not a C return
; value.

default abs

section .bss
align 8
; Milestone 18 (ADR 0018): global, not file-local -- sys_fork
; (kernel/arch/x86_64/syscall.c) needs the user's RSP at the moment of
; the syscall to build the child's synthetic resume trap frame (the
; child's stack VA is identical to the parent's, since fork copies
; every mapping 1:1 by VA -- only the physical frame differs), which
; means C code needs read access to this value; syscall_get_user_rsp()
; is the accessor.
global saved_user_rsp
saved_user_rsp: resq 1

section .text
bits 64

extern syscall_dispatch
extern syscall_kernel_rsp

global syscall_entry
syscall_entry:
    mov [saved_user_rsp], rsp
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

    mov rbx, rsp          ; true frame pointer, preserved across the call (callee-saved)
    mov rdi, rsp          ; arg 1: syscall_frame_t *
    and rsp, ~0xf         ; SysV ABI: RSP must be 16-byte aligned at `call`
    call syscall_dispatch
    mov rsp, rbx          ; restore

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
