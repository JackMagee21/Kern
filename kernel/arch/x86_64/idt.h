#ifndef KERNEL_ARCH_X86_64_IDT_H
#define KERNEL_ARCH_X86_64_IDT_H

#include <stdint.h>

#define IDT_NUM_EXCEPTION_VECTORS 32

/* Interrupt gate: IF is cleared on entry, so a second exception can't
   interrupt a handler that's still deciding how to report the first
   one (CLAUDE.md "keep interrupts-disabled sections as short as
   provable" -- here "as short as" is "until iretq", since every M2
   handler is a terminal fault dump, not a resumable path). */
#define IDT_GATE_TYPE_INTERRUPT_64 0xE

void idt_init(void);

#endif /* KERNEL_ARCH_X86_64_IDT_H */
