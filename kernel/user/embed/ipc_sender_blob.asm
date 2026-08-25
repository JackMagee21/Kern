; Milestone 26 (ADR 0026): embeds the compiled kernel/user/ipc_sender.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global ipc_sender_image_start
global ipc_sender_image_end
ipc_sender_image_start:
incbin "build/kernel/user/ipc_sender.elf"
ipc_sender_image_end:
