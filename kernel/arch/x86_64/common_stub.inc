; Shared by isr.asm and irq.asm: saves all GPRs, calls a C handler with
; RDI = trap_frame_t *, and iretq's back out. Factored out because
; Milestone 5's IRQ stubs need the exact same save/align/restore
; sequence as Milestone 2's exception stubs, and this is the kind of
; small, mechanical, safety-critical asm where a shared NASM macro (DRY
; at the source level) beats either a runtime vector-based dispatch
; trick (would need a hardcoded byte offset into trap_frame_t to read
; the vector field before deciding which C function to call -- fragile,
; breaks silently if the struct layout ever changes) or two independently
; hand-copied blocks that could drift apart.
;
; See isr.asm's original header comment for why the true frame pointer
; is saved in RBX (callee-saved) around the alignment fixup instead of
; just relying on -mno-sse making misalignment harmless.
%macro COMMON_STUB 1 ; %1 = C handler function name (isr_handler / irq_handler)
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
    call %1
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
%endmacro
