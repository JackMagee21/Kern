; Milestone 35 (ADR 0035): embeds the compiled kernel/user/clock_app.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global clock_app_image_start
global clock_app_image_end
clock_app_image_start:
incbin "build/kernel/user/clock_app.elf"
clock_app_image_end:
