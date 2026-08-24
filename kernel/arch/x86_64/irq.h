#ifndef KERNEL_ARCH_X86_64_IRQ_H
#define KERNEL_ARCH_X86_64_IRQ_H

#include <stdint.h>

#include "trap_frame.h"

/* Takes the interrupted trap frame, returns the frame to actually
   resume -- almost always the same one (frame), unless the handler is
   deliberately switching tasks (Milestone 6: kernel/sched/scheduler.c
   registers itself on IRQ0 this way). */
typedef trap_frame_t *(*irq_handler_fn_t)(trap_frame_t *frame);

/* Registers handler to run whenever IRQ irq_line (0-15) fires, called
   from irq_handler (irq_dispatch.c) after the trap frame is saved and
   before pic_send_eoi. No-op if irq_line is out of range. Only one
   handler per line -- registering a second one replaces the first,
   there's no chaining (nothing needs to share a line yet). */
void irq_register_handler(uint8_t irq_line, irq_handler_fn_t handler);

#endif /* KERNEL_ARCH_X86_64_IRQ_H */
