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
        *(COMMON)
        *(.bss*)
    }

    kernel_end_lma = . - KERNEL_VMA_OFFSET;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
