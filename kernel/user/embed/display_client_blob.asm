; Milestone 27 (ADR 0027): embeds the compiled kernel/user/display_client.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global display_client_image_start
global display_client_image_end
display_client_image_start:
incbin "build/kernel/user/display_client.elf"
display_client_image_end:
