#ifndef KERNEL_DRIVERS_CURSOR_H
#define KERNEL_DRIVERS_CURSOR_H

/* Milestone 23 (ADR 0023): draws a small filled square as a mouse
   cursor on the graphics framebuffer, moved by real decoded PS/2 mouse
   deltas (kernel/drivers/mouse.c) -- the thing Milestone 16's mouse
   driver had nothing to draw on top of. Uses a save/restore ("sprite")
   pattern (fb_read_rect()/fb_fill_rect(), kernel/drivers/framebuffer.h)
   so moving the cursor doesn't leave a trail or permanently overwrite
   console text it happens to pass over.

   MUST run after fbconsole_init() -- starts centered on the negotiated
   framebuffer, whose dimensions aren't known any earlier. */
void cursor_init(void);

/* Drains every pending event from mouse.c's dedicated cursor queue
   (mouse_has_cursor_event()/mouse_get_cursor_event() -- NOT the debug
   queue the shell's `mouse` command uses, see mouse.h's doc comment for
   why they're kept independent) and, if the cursor actually moved,
   erases it from its old position and redraws it at the new one.
   Called from shell.c's read_line() wait loop -- every hlt wakeup
   (keyboard, mouse, or the 100Hz timer) gives this a chance to run, so
   the cursor stays responsive without a dedicated polling loop or new
   scheduling primitive. */
void cursor_poll(void);

#endif /* KERNEL_DRIVERS_CURSOR_H */
