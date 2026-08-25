; Milestone 29 (ADR 0029): embeds the compiled kernel/user/input_focus_demo.c
; executable into the kernel image -- same incbin pattern every other
; kernel/user/embed/*_blob.asm file already uses.

section .rodata
global input_focus_demo_image_start
global input_focus_demo_image_end
input_focus_demo_image_start:
incbin "build/kernel/user/input_focus_demo.elf"
input_focus_demo_image_end:
