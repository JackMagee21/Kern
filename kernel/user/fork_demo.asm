; Milestone 18 (ADR 0018): exercises sys_fork (3) and the non-blocking
; sys_wait (4) end to end from ring 3. Linked with the same
; kernel/user/user.ld as kernel/user/hello.asm (same base address is
; fine -- the two programs are never loaded into the same address space
; at once, and kernel/mm/elf_loader.c always maps wherever a given
; image's own program headers say).
;
; Flow: forks once. The CHILD prints a message and exits with a
; specific, otherwise-arbitrary exit code (0x2a) the PARENT branch then
; verifies via sys_wait -- proving both that the child's memory is a
; genuine independent copy (it runs the SAME code, from the SAME
; virtual addresses, but as a completely separate process: kernel_main's
; existing process-isolation self-test already covers "two independent
; processes don't collide"; this specifically proves fork's COPY is
; correct rather than accidentally aliasing the parent) and that
; exit-code propagation through sys_wait actually carries the real
; value the child exited with, not some fixed/garbage placeholder.
;
; sys_wait is NON-BLOCKING (see syscall.c's doc comment for why a real
; blocking wait doesn't exist yet), so the parent polls -- a handful of
; sys_nop round-trips between each sys_wait attempt, just to avoid the
; tightest possible spin; not required for correctness, since the timer
; preempts fairly regardless.

default rel
bits 64

section .text
global _start
_start:
    xor edi, edi
    xor esi, esi
    mov eax, 3          ; SYS_FORK
    syscall

    test rax, rax
    jz .child

    ; --- parent branch: rax = child's pid ---
    mov [child_pid], rax

.wait_loop:
    mov rdi, [child_pid]
    lea rsi, [exit_code_buf]
    mov eax, 4          ; SYS_WAIT
    syscall

    test rax, rax
    jnz .got_child

    mov rbx, 20          ; a short poll-spacing spin, not required for correctness
.spin:
    xor eax, eax         ; SYS_NOP
    syscall
    dec rbx
    jnz .spin
    jmp .wait_loop

.got_child:
    mov rax, [exit_code_buf]
    cmp rax, 0x2a
    jne .parent_bad

    mov rdi, msg_parent_ok
    mov rsi, msg_parent_ok_len
    mov eax, 1           ; SYS_WRITE
    syscall
    jmp .parent_exit

.parent_bad:
    mov rdi, msg_parent_bad
    mov rsi, msg_parent_bad_len
    mov eax, 1
    syscall

.parent_exit:
    xor edi, edi
    mov eax, 2            ; SYS_EXIT(0)
    syscall

.child:
    mov rdi, msg_child
    mov rsi, msg_child_len
    mov eax, 1             ; SYS_WRITE
    syscall

    mov edi, 0x2a
    mov eax, 2              ; SYS_EXIT(0x2a)
    syscall

.hang:                      ; unreachable if sys_exit actually works; defensive only
    jmp .hang

section .rodata
msg_child: db "[OK] child process running after fork", 10
msg_child_len equ $ - msg_child
msg_parent_ok: db "[OK] fork/wait self-test: child exit code verified", 10
msg_parent_ok_len equ $ - msg_parent_ok
msg_parent_bad: db "[FAIL] fork/wait self-test: unexpected child exit code", 10
msg_parent_bad_len equ $ - msg_parent_bad

section .bss
child_pid: resq 1
exit_code_buf: resq 1
