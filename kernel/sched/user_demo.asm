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
; Proves three things: sys_write (validated user-pointer syscall, one-
; shot), sys_nop (no validation needed, looped a bounded number of
; times so kernel/kernel.c's self-test can watch syscall.c's global
; syscall counter climb the same way Milestone 6's demo tasks proved
; forced preemption by never voluntarily yielding), and sys_exit
; (ADR 0010, process lifecycle -- proves a process can actually
; terminate and be reaped, not just run forever like Milestone 7/9's
; version did).
;
; LOOP_COUNT lives in RBX, not RCX -- the SYSCALL instruction itself
; clobbers RCX (return RIP) and R11 (return RFLAGS), so neither can
; hold state that needs to survive across a `syscall`. Every OTHER GPR,
; including RBX, is saved/restored symmetrically by syscall_entry.asm
; around the call into syscall_dispatch, and SYS_NOP's handler never
; touches it either.

default rel

section .user_demo
bits 64

global user_demo_start
user_demo_start:
    lea rdi, [msg]
    mov rsi, msg_len
    mov rax, 1          ; SYS_WRITE (kernel/arch/x86_64/syscall.h)
    syscall

    mov rbx, LOOP_COUNT
.loop:
    xor eax, eax        ; SYS_NOP
    syscall
    dec rbx
    jnz .loop

    mov eax, 2          ; SYS_EXIT -- never returns
    syscall

.hang:                  ; unreachable if sys_exit actually works; defensive only
    jmp .hang

msg: db "[OK] hello from ring 3 via syscall", 10
msg_len equ $ - msg

LOOP_COUNT equ 200000
