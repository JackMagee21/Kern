; gdt_flush(gdt_ptr_t *ptr): lgdt, then reload every segment register.
; CS can't be reloaded with a plain `mov` -- only a far jump/call/ret (or
; iretq) changes it -- so this uses the standard retfq trick: push the
; new CS selector and a return address, then retfq pops both and
; performs the far jump in one step.

default abs

section .text
bits 64

global gdt_flush

gdt_flush:
    lgdt [rdi]

    mov ax, 0x10          ; KERNEL_DATA_SELECTOR (gdt.h)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push qword 0x08        ; KERNEL_CODE_SELECTOR (gdt.h)
    lea rax, [rel .flushed]
    push rax
    retfq
.flushed:
    ret
