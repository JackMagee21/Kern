; Milestone 18 (ADR 0018): embeds the compiled kernel/user/fork_demo.asm
; executable into the kernel image, exactly the same pattern
; user_elf_blob.asm already uses for hello.elf -- see that file's doc
; comment for why no special page isolation is needed here either. Kept
; as its own file (rather than a second incbin in user_elf_blob.asm) so
; each embedded image has its own single, obviously-paired asm file.

section .rodata
global fork_demo_image_start
global fork_demo_image_end
fork_demo_image_start:
incbin "build/kernel/user/fork_demo.elf"
fork_demo_image_end:
