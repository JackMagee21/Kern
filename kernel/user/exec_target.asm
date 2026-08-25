; Milestone 22 (ADR 0022): the image kernel/user/exec_demo.asm sys_execs
; into (program id 0). Deliberately a completely separate program from
; exec_demo.asm -- not just a jump within one file -- so kernel/mm/
; elf_loader.c genuinely reloads a whole new set of PT_LOAD segments
; into the process's (reset) address space, and deliberately NOT a reuse
; of hello.asm/fork_demo.asm's own images, since those two programs'
; existing self-tests count their own messages an EXACT number of times
; (kernel_main / tests/qemu/test_elf_loader_selftest.sh) -- reusing
; either here would silently perturb those counts instead of proving
; anything new.
;
; Its own message and exit code are both unique to this file, so its
; appearance in the boot log unambiguously proves sys_exec genuinely
; swapped the running process's code, not just that SOME syscall
; returned successfully.

default rel
bits 64

section .text
global _start
_start:
    mov rdi, msg_target
    mov rsi, msg_target_len
    mov eax, 1           ; SYS_WRITE
    syscall

    mov edi, 0x37          ; distinct exit code -- proves this specific
                            ; program (not exec_demo's own image) ran to
                            ; completion
    mov eax, 2               ; SYS_EXIT
    syscall

.hang:                       ; unreachable if sys_exit actually works; defensive only
    jmp .hang

section .rodata
msg_target: db "[OK] exec target running -- process image was genuinely replaced by sys_exec", 10
msg_target_len equ $ - msg_target
