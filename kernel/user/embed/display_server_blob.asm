; Milestone 27 (ADR 0027): embeds the compiled kernel/user/display_server.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global display_server_image_start
global display_server_image_end
display_server_image_start:
incbin "build/kernel/user/display_server.elf"
display_server_image_end:
