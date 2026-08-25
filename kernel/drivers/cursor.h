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

/* Milestone 28 (ADR 0028): erases the cursor sprite (restoring the
   real pixels underneath it) if currently visible; a no-op if it's
   already hidden or cursor_init() hasn't run yet. Exists specifically
   so a caller about to perform a BULK framebuffer operation that
   doesn't know or care about the cursor (fbconsole.c's own
   fb_scroll_up() call, the first real user) can make sure that
   operation never has to reason about a sprite sitting on top of the
   content it's about to shift -- restoring a save/restore buffer
   AFTER such an operation, or leaving an old sprite un-erased at a
   position the operation just invalidated, is exactly what a real
   ghost-trail bug in this driver looked like before this existed (see
   ADR 0028's Verification). Always pair with cursor_show() afterward. */
void cursor_hide(void);

/* Milestone 28 (ADR 0028): redraws the cursor sprite at its current
   logical position, capturing a FRESH background first -- correct to
   call any time after cursor_hide(), regardless of what changed on
   screen in between. A no-op if cursor_init() hasn't run yet. */
void cursor_show(void);

#endif /* KERNEL_DRIVERS_CURSOR_H */
