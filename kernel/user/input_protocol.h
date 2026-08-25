#ifndef KERNEL_USER_INPUT_PROTOCOL_H
#define KERNEL_USER_INPUT_PROTOCOL_H

/* Milestone 29 (ADR 0029): the wire format for a hardware input event
   the KERNEL itself delivers to whichever ring-3 process has
   subscribed (sys_input_subscribe(), kernel/arch/x86_64/syscall.c) --
   via the SAME ipc_message_t/ipc_send() mechanism kernel_main's own
   bootstrap messages already use (Milestone 26), just triggered by a
   real IRQ12 mouse event instead of kernel_main's own startup code.
   Defined ONCE here and shared, unmodified, by both kernel-side code
   (kernel/drivers/input_router.c) and userspace code (kernel/user/
   input_focus_demo.c, and eventually kernel/user/display_server.c) --
   the same "one shared definition, not duplicated on each side"
   reasoning kernel/ipc/ipc_message.h's own doc comment already gives
   for why a wire-format mismatch is worth preventing structurally, not
   just by convention. Lives under kernel/user/ (like
   display_protocol.h) rather than kernel/ipc/, since the GENERIC IPC
   mechanism itself doesn't know or care what this specific opcode
   means -- only this protocol's own kernel and userspace ends do. */

/* fields[1] = x, fields[2] = y (screen pixel coordinates, the same
   ones kernel/drivers/cursor.c's own cursor_x/cursor_y track) at the
   moment of a real left-mouse-button-DOWN edge -- a transition,
   detected by cursor.c, not a level (a button HELD down across
   multiple mouse reports fires this exactly once, on the first report
   where it reads pressed). fields[3] unused. */
#define INPUT_EVENT_CLICK 1

/* Milestone 31 (ADR 0031): fields[1] = x, fields[2] = y -- the cursor's
   CURRENT position, sent once per kernel/drivers/cursor.c poll where
   the cursor moved WHILE the left button was already held down (i.e.
   NOT the same report as the initial press -- that's still
   INPUT_EVENT_CLICK). Lets a subscriber implement drag-to-move without
   a general "stream every mouse move" event kernel_main's own
   kernel/drivers/mouse.c debug queue already proves nothing else needs
   (only sent while a button is down, never on a plain hover). */
#define INPUT_EVENT_DRAG 2

/* Milestone 31 (ADR 0031): fields[1] = x, fields[2] = y -- sent once,
   on the report where the left button transitions from held to
   released (the mirror image of INPUT_EVENT_CLICK's press edge). */
#define INPUT_EVENT_RELEASE 3

#endif /* KERNEL_USER_INPUT_PROTOCOL_H */
