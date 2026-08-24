; Milestone 17: embeds the compiled userspace ELF64 executable
; (build/kernel/user/hello.elf, kernel/user/hello.asm + user.ld) into
; the kernel image as ordinary read-only data. Unlike Milestone 7-16's
; user_demo.asm (retired), this blob is NEVER mapped into a process's
; address space directly or executed in place -- kernel/mm/elf_loader.c
; only ever reads its bytes through an ordinary kernel pointer (the
; kernel's own .rodata is already mapped, shared, supervisor-only,
; readable from any context) and copies validated PT_LOAD segment
; content into freshly allocated per-process frames. So, unlike
; user_demo.asm, this needs no special page isolation/alignment of its
; own -- it can sit inline in the kernel's normal .rodata output
; section alongside everything else.
;
; The Makefile builds build/kernel/user/hello.elf BEFORE assembling this
; file (explicit dependency) -- incbin's path is relative to nasm's
; invocation directory (the repo root, same as every other `nasm -f
; elf64` invocation in this Makefile).

section .rodata
global user_elf_image_start
global user_elf_image_end
user_elf_image_start:
incbin "build/kernel/user/hello.elf"
user_elf_image_end:
