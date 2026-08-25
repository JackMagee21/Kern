; Milestone 24: entry point for ring-3 programs compiled from C via the
; minimal userspace runtime (kernel/user/rt/). Bridges from the raw
; entry context task.c/task_exec() hands every process (RSP already
; pointing at a valid, 16-byte-aligned user stack top --
; USER_STACK_VIRT_BASE/USER_STACK_SIZE, kernel/sched/task.c, both
; 16-byte-aligned constants -- but with NO return address pushed, since
; this is entered via iretq/sysretq, not `call`) into a context main()
; can safely run in as an ordinary SysV-ABI C function.
;
; A C function's own prologue assumes it was entered via `call` (RSP%16
; == 8 at entry, accounting for the pushed return address) -- since
; nothing pushed a return address here, RSP%16 == 0 instead. Explicitly
; re-aligning before the `call` below is the exact same fix
; kernel/arch/x86_64/boot.asm's own higher_half_entry already applies
; before ITS first C call (`and rsp, ~0xf`) for the identical reason --
; not a new pattern, the established one for this exact class of
; boundary.
default abs
bits 64

section .text
global _start
extern main
extern sys_exit

_start:
    and rsp, ~0xf
    call main
    mov edi, eax    ; sys_exit(main()'s return value)
    call sys_exit   ; noreturn

.hang:              ; unreachable if sys_exit actually works; defensive only
    jmp .hang
