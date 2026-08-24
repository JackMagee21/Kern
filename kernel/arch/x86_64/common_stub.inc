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
;
; Per-process address spaces (scheduler.c): the CR3 switch itself has
; to happen HERE, in the assembly, after `mov rsp, rax` -- not inside
; the C handler. Found the hard way (first real boot triple-faulted):
; timer_tick_handler used to switch CR3 itself, but at that point it
; was still running on the OUTGOING task's stack, mid-function -- if
; that stack is bootstrap's low boot_stack (identity-mapped only under
; the kernel's own table, not a process's), the very next stack access
; after the switch (the C function's own epilogue, before it even
; returns) faults, because the just-loaded process page table has no
; identity mapping. After `mov rsp, rax`, RSP points at the INCOMING
; task's own saved frame, which is always reachable regardless of which
; CR3 was loaded a moment ago: a kernel thread's or process's
; kernel_stack_top is ordinary kmalloc'd memory under the shared
; PML4[511] kernel half, present in every address space. That's the
; only point in this whole path guaranteed safe to change CR3 at.
extern scheduler_current_pml4
extern scheduler_target_pml4

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

    ; Now safe to switch CR3 (see comment above). Skip the actual write
    ; when nothing changed (most ticks, since kernel threads share one
    ; address space) -- reloading CR3 always flushes the entire
    ; non-global TLB, so an unconditional reload would do that on every
    ; single 100Hz tick for no reason.
    mov rax, [scheduler_target_pml4]
    mov rcx, [scheduler_current_pml4]
    cmp rax, rcx
    je %%no_pml4_switch
    mov [scheduler_current_pml4], rax
    mov cr3, rax
%%no_pml4_switch:

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
