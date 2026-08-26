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
   IDT vector (32-47); the IRQ line is that minus IDT_IRQ_VECTOR_BASE.
   Returns whichever frame the registered handler chose to resume
   (almost always the same one it was given -- see irq.h); an
   unregistered line just resumes frame unchanged.

   Milestone 33 follow-up (real hardware, KVM): trap_frame_fixup_ss()
   (trap_frame.h) is applied HERE, unconditionally, to whatever frame is
   about to be resumed -- covering every IRQ line uniformly, not just
   IRQ0. Originally (ADR 0032) it was only called from
   scheduler.c's timer_tick_handler and exceptions.c's isr_handler,
   because the flight-recorder evidence available at the time only
   showed the corruption on a COW #PF frame. A real boot then #GP'd
   while dragging a window: mouse-move generates IRQ12 at a high rate,
   which can interrupt a ring-3 task (e.g. the pulse app) and, on real
   hardware, hit the exact same KVM SS-capture corruption ADR 0032
   already root-caused -- but mouse_irq_handler/keyboard_irq_handler
   (kernel/drivers/mouse.c/keyboard.c) just process the byte and return
   the SAME frame unchanged, with no task switch and no fixup, so the
   corrupted ss was iretq-ing verbatim. Fixing it here, once, for every
   IRQ line (rather than adding a call inside each of mouse.c/
   keyboard.c/any future IRQ handler) is the same "every path that can
   hand a frame to iretq" invariant ADR 0032 already established, just
   applied at the one choke point common to all of them. Idempotent and
   cheap (a single branch) when the frame was already correct, so this
   is a no-op for the ring-0-interrupted-by-IRQ case (frame->cs RPL !=
   3) and for every IRQ0 timer switch, whose own now-redundant call
   inside timer_tick_handler was removed in favor of this single site. */
trap_frame_t *irq_handler(trap_frame_t *frame)
{
    uint8_t irq_line = (uint8_t)(frame->vector - IDT_IRQ_VECTOR_BASE);
    trap_frame_t *resume = frame;

    if (irq_line < IDT_NUM_IRQ_VECTORS && handlers[irq_line] != NULL) {
        resume = handlers[irq_line](frame);
    }

    pic_send_eoi(irq_line);
    trap_frame_fixup_ss(resume); /* see trap_frame.h's own doc comment, and this function's above */
    return resume;
}
