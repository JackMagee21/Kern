; Milestone 22 (ADR 0022): embeds the compiled kernel/user/exec_demo.asm
; executable into the kernel image -- same incbin pattern
; kernel/user/embed/user_elf_blob.asm/fork_demo_blob.asm already use, see
; user_elf_blob.asm's doc comment for why no special page isolation is
; needed here either.

section .rodata
global exec_demo_image_start
global exec_demo_image_end
exec_demo_image_start:
incbin "build/kernel/user/exec_demo.elf"
exec_demo_image_end:
