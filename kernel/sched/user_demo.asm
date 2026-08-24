; Milestone 7: the ring-3 demo program. Hand-written NASM, not compiled
; C, because it must be genuinely position-independent -- it's linked
; at a normal kernel higher-half address (boot/linker.ld places
; .user_demo there, padded to its own page(s)), but task_create_user()
; (kernel/sched/task.c) maps that SAME physical page a second time, at
; a completely different user-region virtual address with the U/S bit
; set, and that second mapping is the one this code actually executes
; from. `default rel` makes every internal reference RIP-relative, so
; the code works correctly regardless of which of its two virtual
; aliases it's running through. It never references anything outside
; itself (syscall needs no address -- the CPU jumps to LSTAR
; automatically), so this is the only position-independence guarantee
; it needs.
;
; Proves two things: sys_write (validated user-pointer syscall, one-
; shot) and sys_nop (no validation needed, looped forever) -- the loop
; deliberately never stops, so kernel/kernel.c's self-test can watch
; syscall.c's global syscall counter climb the same way Milestone 6's
; demo tasks proved forced preemption by never voluntarily yielding.

default rel

section .user_demo
bits 64

global user_demo_start
user_demo_start:
    lea rdi, [msg]
    mov rsi, msg_len
    mov rax, 1          ; SYS_WRITE (kernel/arch/x86_64/syscall.h)
    syscall

.loop:
    xor eax, eax        ; SYS_NOP
    syscall
    jmp .loop

msg: db "[OK] hello from ring 3 via syscall", 10
msg_len equ $ - msg
