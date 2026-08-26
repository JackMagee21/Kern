; Milestone 33 (ADR 0033): embeds the compiled kernel/user/pulse_app.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global pulse_app_image_start
global pulse_app_image_end
pulse_app_image_start:
incbin "build/kernel/user/pulse_app.elf"
pulse_app_image_end:
