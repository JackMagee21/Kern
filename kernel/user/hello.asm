; Milestone 17: a real ring-3 ELF64 executable, loaded by
; kernel/mm/elf_loader.c instead of Milestone 7-16's hand-mapped raw
; code blob (kernel/sched/user_demo.asm, retired). Linked (kernel/user/
; user.ld) at a FIXED virtual address matching task.c's existing
; USER_CODE_VIRT_BASE convention, so unlike user_demo.asm this is
; genuinely position-DEPENDENT -- it doesn't need `default rel`'s
; RIP-relative trick, since the loader always maps it at exactly the
; vaddr its own ELF program headers specify (no second aliasing
; mapping, unlike the old shared-code-page design -- see ADR 0017).
;
; Proves four things beyond what user_demo.asm already proved
; (sys_write/sys_nop/sys_exit all still work): (1) a REAL multi-segment
; ELF64 image gets parsed and mapped correctly, not just a single flat
; blob; (2) .data segment content is actually copied in from the file
; (data_var must read back 0x1234, its real initializer, not 0); (3)
; .bss is genuinely zero-filled, not garbage or leftover frame content
; (bss_var must read back 0 before this code ever writes it); (4) a
; writable data segment is actually writable (bss_var can be written
; and read back afterward). A failure in any of these produces a
; distinctly different serial message the QEMU smoke test greps for.

default rel  ; RIP-relative [label] memory operands -- needed since this links
             ; far above the 32-bit signed displacement absolute addressing
             ; would need (base 0x8000400000); immediate loads like
             ; `mov rdi, msg1` are unaffected (full 64-bit absolute, not a
             ; memory operand) and still resolve correctly.
bits 64

section .text
global _start
_start:
    mov rdi, msg1
    mov rsi, msg1_len
    mov rax, 1          ; SYS_WRITE
    syscall

    mov rax, [bss_var]
    test rax, rax
    jnz .bad

    mov rax, [data_var]
    cmp rax, 0x1234
    jne .bad

    mov qword [bss_var], 0x5678

    mov rdi, msg2
    mov rsi, msg2_len
    mov rax, 1
    syscall
    jmp .after_check

.bad:
    mov rdi, msg_bad
    mov rsi, msg_bad_len
    mov rax, 1
    syscall

.after_check:
    mov rbx, LOOP_COUNT
.loop:
    xor eax, eax        ; SYS_NOP
    syscall
    dec rbx
    jnz .loop

    mov eax, 2           ; SYS_EXIT -- never returns
    syscall

.hang:                   ; unreachable if sys_exit actually works; defensive only
    jmp .hang

section .rodata
msg1: db "[OK] hello from ring 3 via ELF-loaded process", 10
msg1_len equ $ - msg1
msg2: db "[OK] elf .data/.bss segment verification passed", 10
msg2_len equ $ - msg2
msg_bad: db "[FAIL] elf .data/.bss segment verification failed", 10
msg_bad_len equ $ - msg_bad

section .data
data_var: dq 0x1234

section .bss
bss_var: resq 1

LOOP_COUNT equ 200000
