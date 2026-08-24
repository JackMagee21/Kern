/*
 * Milestone 17: linker script for kernel/user/hello.asm, producing a
 * real static ELF64 executable (build/kernel/user/hello.elf) that
 * kernel/mm/elf_loader.c parses and maps -- NOT linked against the
 * kernel image itself (a completely separate `x86_64-elf-ld -T` link,
 * see Makefile), and NOT built with -mcmodel=kernel or any kernel
 * compiler flags, since this is ordinary ring-3 user code.
 *
 * Base address matches kernel/sched/task.c's existing
 * USER_CODE_VIRT_BASE (0x8000400000) -- not load-bearing for
 * correctness (elf_loader.c maps wherever a segment's p_vaddr says),
 * just keeps the address stable/familiar across the Milestone 7->17
 * transition. Each output section gets its own explicit ALIGN(4K):
 * elf_loader.c's simplifying assumption (libk/elf.h's
 * elf64_validate_load_segment) requires every PT_LOAD segment's
 * p_vaddr to be page-aligned, which only holds if the sections making
 * up each segment start on their own page -- GNU ld's default script
 * doesn't guarantee that for a small executable like this one.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x8000400000;

    .text ALIGN(4K) : { *(.text*) }

    .rodata ALIGN(4K) : { *(.rodata*) }

    .data ALIGN(4K) : { *(.data*) }

    .bss ALIGN(4K) : { *(.bss*) }

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
