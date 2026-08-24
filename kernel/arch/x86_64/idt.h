#ifndef KERNEL_ARCH_X86_64_IDT_H
#define KERNEL_ARCH_X86_64_IDT_H

#include <stdint.h>

#define IDT_NUM_EXCEPTION_VECTORS 32

/* Milestone 5: PIC remapped so IRQ0-15 land on vectors 32-47 (see
   kernel/drivers/pic.c) instead of colliding with the CPU exception
   vectors above. */
#define IDT_IRQ_VECTOR_BASE 32
#define IDT_NUM_IRQ_VECTORS 16

/* Interrupt gate: IF is cleared on entry, so a second exception/IRQ
   can't interrupt a handler that's still running (CLAUDE.md "keep
   interrupts-disabled sections as short as provable" -- for exceptions
   that's "until iretq" since every M2 handler is a terminal fault dump;
   for IRQs the handler body is trivially short (increment a counter,
   send EOI), so the same gate type is fine there too). */
#define IDT_GATE_TYPE_INTERRUPT_64 0xE

void idt_init(void);

#endif /* KERNEL_ARCH_X86_64_IDT_H */
