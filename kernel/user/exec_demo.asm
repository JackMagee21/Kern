; Milestone 22 (ADR 0022): exercises sys_exec (5) end to end from ring 3.
; Linked with the same kernel/user/user.ld as every other embedded
; program (same base address is fine -- never loaded into the same
; address space as its own target at once, same reasoning fork_demo.asm's
; own doc comment already gives).
;
; Flow: prints a message, then execs into kernel/user/exec_target.asm
; (program id 0, kernel/sched/task.c's exec_lookup_image()). If sys_exec
; actually works, NOTHING below the syscall in THIS file ever runs again
; -- the process's entire image (code/data/bss/stack) has been replaced
; and execution resumes at exec_target.asm's own _start instead. The
; "unreachable" message below only exists so a BROKEN sys_exec (one that
; returns control to the old image instead of actually replacing it)
; produces a visibly distinct, greppable failure rather than silently
; falling through to whatever garbage instruction happens to follow.
;
; Proving "still the same process, not a new one" doesn't need any
; cross-image coordination here (a register/stack convention would be
; fragile and isn't what real exec()'s ABI does anyway) -- kernel_main's
; own reap-count accounting already proves it more rigorously: this
; process is spawned ONCE, and exactly ONE "exited and was reaped"
; message must appear for it, whichever image it happens to be running
; when it finally calls sys_exit (see exec_target.asm).

default rel
bits 64

section .text
global _start
_start:
    mov rdi, msg_before
    mov rsi, msg_before_len
    mov eax, 1          ; SYS_WRITE
    syscall

    xor edi, edi         ; program_id 0 = EXEC_PROGRAM_TARGET
    mov eax, 5           ; SYS_EXEC
    syscall

    ; Only reached if sys_exec FAILED (returned -1 in rax) -- a working
    ; sys_exec never falls through to here at all.
    mov rdi, msg_fail
    mov rsi, msg_fail_len
    mov eax, 1
    syscall

    mov edi, 0xee         ; distinct failure exit code
    mov eax, 2             ; SYS_EXIT
    syscall

.hang:                     ; unreachable if sys_exit actually works; defensive only
    jmp .hang

section .rodata
msg_before: db "[OK] exec demo running, about to sys_exec into a new image", 10
msg_before_len equ $ - msg_before
msg_fail: db "[FAIL] sys_exec returned control to the old image (should be unreachable)", 10
msg_fail_len equ $ - msg_fail
