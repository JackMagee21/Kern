#include <stddef.h>
#include <stdint.h>

#include "idt.h"
#include "irq.h"
#include "trap_frame.h"
#include "../../drivers/pic.h"

static irq_handler_fn_t handlers[IDT_NUM_IRQ_VECTORS];

void irq_register_handler(uint8_t irq_line, irq_handler_fn_t handler)
{
    if (irq_line < IDT_NUM_IRQ_VECTORS) {
        handlers[irq_line] = handler;
    }
}

/* Called from irq.asm's irq_common_stub. frame->vector is the literal
   IDT vector (32-47); the IRQ line is that minus IDT_IRQ_VECTOR_BASE. */
void irq_handler(trap_frame_t *frame)
{
    uint8_t irq_line = (uint8_t)(frame->vector - IDT_IRQ_VECTOR_BASE);

    if (irq_line < IDT_NUM_IRQ_VECTORS && handlers[irq_line] != NULL) {
        handlers[irq_line]();
    }

    pic_send_eoi(irq_line);
}
