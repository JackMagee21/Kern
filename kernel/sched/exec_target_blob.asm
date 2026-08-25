; Milestone 22 (ADR 0022): embeds the compiled kernel/user/exec_target.asm
; executable into the kernel image -- referenced by kernel/sched/task.c's
; exec_lookup_image() (program_id 0), NEVER by task_create_user_image()
; directly -- this image is only ever reached via sys_exec, not spawned
; as its own top-level process.

section .rodata
global exec_target_image_start
global exec_target_image_end
exec_target_image_start:
incbin "build/kernel/user/exec_target.elf"
exec_target_image_end:
