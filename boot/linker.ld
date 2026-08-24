/*
 * Milestone 1: Boot -> "hello kernel" via serial.
 *
 * Higher-half layout: GRUB2 loads the image at physical 1MiB and hands
 * control to 32-bit protected-mode code that must run at its *load*
 * address (paging is off). That trampoline (.boot) is therefore kept at
 * a low, identity VMA==LMA. Once boot.asm enables long mode and identity-
 * maps the low 8MiB region at both VA=PA and VA=PA+KERNEL_VMA_OFFSET
 * (see kernel/arch/x86_64/boot.asm), the rest of the kernel (.text/.rodata/
 * .data/.bss, compiled with -mcmodel=kernel) links at the top-2GB virtual
 * base that mcmodel=kernel assumes, while its LMA (AT>) keeps it
 * contiguous in physical memory right after the boot trampoline.
 *
 * __bss_start/__bss_end (Milestone 3): GRUB only loads the file's real
 * (PROGBITS) content -- nothing guarantees the physical RAM backing
 * .bss is actually zero, since it's NOLOAD. boot.asm zeroes exactly
 * this range before calling kernel_main, so kernel/mm/pmm.c's static
 * bitmap (and any other kernel static) can rely on C's normal
 * zero-initialization guarantee instead of every subsystem having to
 * self-initialize defensively.
 */

ENTRY(_start)

KERNEL_VMA_OFFSET = 0xFFFFFFFF80000000;
KERNEL_LMA_START  = 0x100000; /* 1 MiB: where GRUB2 loads the image */

SECTIONS
{
    . = KERNEL_LMA_START;

    /* Multiboot2 header + 32-bit boot trampoline: identity VMA==LMA,
       must run with paging disabled. */
    .boot ALIGN(4K) :
    {
        KEEP(*(.multiboot2))
        *(.boot.text)
        *(.boot.rodata)
        *(.boot.data)
    }

    .boot.bss ALIGN(4K) (NOLOAD) :
    {
        *(.boot.bss)
    }

    /* Everything from here on links at the higher-half virtual base;
       AT() keeps the load address contiguous in low physical memory. */
    . += KERNEL_VMA_OFFSET;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA_OFFSET)
    {
        *(.text*)
    }

    /* Milestone 7: the ring-3 demo program's raw machine code
       (kernel/sched/user_demo.asm), isolated on its own page(s) --
       padded to a full 4KiB boundary on both sides so that when
       task_create_user() maps this physical range into the dedicated
       user virtual region with the U/S bit set, no OTHER kernel code or
       data sharing the same physical page gets exposed to ring 3 along
       with it (the U bit is a per-4KiB-frame grant, not
       per-symbol/per-section). See ADR 0007. */
    .user_demo ALIGN(4K) : AT(ADDR(.user_demo) - KERNEL_VMA_OFFSET)
    {
        *(.user_demo)
        . = ALIGN(4K);
    }
    user_demo_start_lma = ADDR(.user_demo) - KERNEL_VMA_OFFSET;
    user_demo_end_lma   = ADDR(.user_demo) + SIZEOF(.user_demo) - KERNEL_VMA_OFFSET;

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA_OFFSET)
    {
        *(.rodata*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA_OFFSET)
    {
        *(.data*)
    }

    .bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA_OFFSET)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }

    kernel_end_lma = . - KERNEL_VMA_OFFSET;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
