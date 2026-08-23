; Milestone 2: IDT + exception handlers.
;
; One stub per CPU exception vector (0-31), normalizing every vector to
; the same stack shape (a real or dummy error code, then the vector
; number) before falling into a shared handler that saves all GPRs,
; calls the C fault handler, and iretq's back out.
;
; Error-code vectors verified against the Wikipedia "Interrupt
; descriptor table" x86 exception table (8, 10, 11, 12, 13, 14, 17, 21
; push a hardware error code; the rest do not; 22-31 beyond that are
; reserved/rare and treated as no-error-code, which is inconsequential
; since real hardware won't raise them for this kernel).
;
; Long mode always pushes SS:RSP as part of the exception frame (unlike
; legacy protected mode) specifically so the CPU can guarantee RSP is
; 16-byte aligned at the first instruction of the handler, regardless of
; whether an error code was also pushed (see trap_frame.h for the
; verification caveat on this). That guarantee is why isr_common_stub
; below does NOT try to make both push-paths land at the same alignment
; by construction (8 vs 16 bytes pushed before the vector number: 8
; doesn't evenly divide against a shared fixed-size GPR push count for
; both parities) -- instead it saves the true stack pointer in RBX (a
; callee-saved register, so the C handler is required to preserve it)
; before forcibly aligning RSP for the `call`, and restores RSP from RBX
; afterward. This is CLAUDE.md's flagged gotcha ("a push in the stub
; shifts alignment; account for it") handled explicitly, not by luck.

default abs

section .text
bits 64

extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common_stub
%endmacro

isr_common_stub:
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
    mov rdi, rsp          ; arg 1: trap_frame_t *
    and rsp, ~0xf         ; SysV ABI: RSP must be 16-byte aligned at `call`
    call isr_handler
    mov rsp, rbx          ; restore the true frame pointer

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

    add rsp, 16            ; discard vector number + error code
    iretq

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31
