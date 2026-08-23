#ifndef KERNEL_ARCH_X86_64_GDT_H
#define KERNEL_ARCH_X86_64_GDT_H

/*
 * Same 3-entry flat layout (null, kernel code, kernel data) that
 * boot.asm's provisional gdt64 used to get into long mode -- this is
 * the "real" GDT kernel_main switches to once C is running, so it's a
 * C-managed table like GDT/IDT entries elsewhere in this kernel, per
 * CLAUDE.md's explicit-bit-packed-layout rule, instead of the hand
 * assembled table that only exists to bootstrap this one.
 *
 * No TSS and no ring-3 descriptors yet: neither is load-bearing until
 * IST stacks or userspace show up (Milestones covering those), and
 * adding them now would be scope creep ahead of what's needed.
 */
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10

void gdt_init(void);

#endif /* KERNEL_ARCH_X86_64_GDT_H */
