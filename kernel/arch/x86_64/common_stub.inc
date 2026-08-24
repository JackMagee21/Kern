; Shared by isr.asm and irq.asm: saves all GPRs, calls a C handler with
; RDI = trap_frame_t *, and resumes whichever frame the handler RETURNS
; (RAX) rather than always resuming the one it was given.
;
; This is what makes Milestone 6's preemptive context switch possible
; with no separate save/restore mechanism: because every task's saved
; state is already a complete trap_frame_t sitting on that task's own
; stack (either from a real interrupt, or a synthetic one task_create()
; built to look like one -- see kernel/sched/task.c), a "context switch"
; is nothing more than the timer's handler returning a DIFFERENT
; trap_frame_t* than the one it received. isr_handler (exceptions.c)
; always returns the same frame it was given -- exceptions never trigger
; a task switch -- so Milestones 2/5's behavior is unchanged; only
; irq_dispatch.c's timer path can actually return something different.
;
; Factored into a shared macro (not duplicated between isr.asm/irq.asm,
; not a runtime vector-based dispatch trick reading a hardcoded offset
; into trap_frame_t) for the same reasons as before: DRY at the source
; level, simple and self-contained at the generated-code level.
;
; See isr.asm's original header comment for why RSP must be forced
; 16-byte aligned before `call` regardless of which push-path got here.
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

    mov rdi, rsp          ; arg 1: trap_frame_t *
    and rsp, ~0xf         ; SysV ABI: RSP must be 16-byte aligned at `call`
    call %1               ; returns the trap_frame_t * to resume (RAX)
    mov rsp, rax          ; switch to whichever frame the handler selected

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
