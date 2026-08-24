; Milestone 5: PIT/PIC timer + IRQ handling.
;
; One stub per remapped IRQ vector (32-47, see kernel/drivers/pic.c),
; sharing common_stub.inc's save/align/restore sequence with isr.asm's
; exception stubs. Hardware IRQs never push an error code (only the
; specific CPU exception vectors ISR_ERR covers in isr.asm do), so every
; IRQ stub pushes a dummy 0 to keep the same trap_frame_t shape, then the
; VECTOR NUMBER (32+line, not the bare 0-15 line number) -- trap_frame_t
; .vector is always the literal IDT vector that fired, uniformly across
; both isr.asm and this file; irq_handler (irq_dispatch.c) subtracts
; IDT_IRQ_VECTOR_BASE itself to recover the 0-15 line number.

default abs

%include "kernel/arch/x86_64/common_stub.inc"

section .text
bits 64

extern irq_handler

%macro IRQ_STUB 2 ; %1 = IRQ line (0-15), %2 = IDT vector (32-47)
global irq%1
irq%1:
    push qword 0
    push qword %2
    jmp irq_common_stub
%endmacro

irq_common_stub:
    COMMON_STUB irq_handler

IRQ_STUB 0, 32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47
