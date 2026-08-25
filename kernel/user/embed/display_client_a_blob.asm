; Milestone 28 (ADR 0028): embeds the compiled kernel/user/display_client_a.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global display_client_a_image_start
global display_client_a_image_end
display_client_a_image_start:
incbin "build/kernel/user/display_client_a.elf"
display_client_a_image_end:
