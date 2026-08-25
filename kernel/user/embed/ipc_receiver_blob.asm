; Milestone 26 (ADR 0026): embeds the compiled kernel/user/ipc_receiver.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global ipc_receiver_image_start
global ipc_receiver_image_end
ipc_receiver_image_start:
incbin "build/kernel/user/ipc_receiver.elf"
ipc_receiver_image_end:
