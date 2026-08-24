#ifndef KERNEL_DRIVERS_PIC_H
#define KERNEL_DRIVERS_PIC_H

#include <stdint.h>

/* Remaps the 8259 PIC's IRQ0-15 from their power-on default (vectors
   0-15, colliding with the CPU exception vectors) to 32-47, then masks
   every line -- callers must explicitly pic_clear_mask() the lines they
   actually handle. Must run before idt_init()'s IRQ gates are used, and
   before sti. */
void pic_remap(void);

/* Sends End-Of-Interrupt for irq_line (0-15). Must be called at the end
   of every IRQ handler, or the PIC will never signal that line again. */
void pic_send_eoi(uint8_t irq_line);

void pic_set_mask(uint8_t irq_line);
void pic_clear_mask(uint8_t irq_line);

#endif /* KERNEL_DRIVERS_PIC_H */
