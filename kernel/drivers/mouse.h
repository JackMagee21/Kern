#ifndef KERNEL_DRIVERS_MOUSE_H
#define KERNEL_DRIVERS_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

/* One decoded movement/button report. dx/dy are raw PS/2-convention
   relative deltas (positive dy = UP, the opposite of typical screen
   coordinates) -- left un-negated since nothing consumes these as
   screen coordinates yet (no graphics/cursor exists, ADR 0008); a
   future display consumer decides its own sign convention when it
   exists. */
typedef struct {
    int16_t dx;
    int16_t dy;
    bool left;
    bool right;
    bool middle;
} mouse_event_t;

/* Enables the PS/2 controller's second ("auxiliary") port, puts the
   mouse into its default streaming-report mode, and registers the
   IRQ12 handler. Does NOT unmask IRQ12 (or IRQ2, the master PIC's
   cascade line IRQ12 needs to ever reach the CPU at all) -- callers do
   that once ready, matching keyboard_init()'s pattern. */
void mouse_init(void);

bool mouse_has_event(void);

/* Returns the next queued event (a zeroed event if the queue is
   empty -- callers that care should check mouse_has_event() first).
   Drops new events once the queue is full, the same "fixed capacity,
   simplest correct behavior" contract as keyboard.c's ring buffer. */
mouse_event_t mouse_get_event(void);

/* Milestone 23 (ADR 0023): a SECOND, independent queue -- every decoded
   packet is broadcast into both this one and the debug queue above
   (mouse_has_event()/mouse_get_event(), used by the shell's own `mouse`
   command). Needed specifically because kernel/drivers/cursor.c's
   cursor_poll() and the shell's `mouse` command now consume the SAME
   underlying hardware stream from two genuinely different call sites
   (cursor_poll() runs continuously from shell.c's read_line() wait
   loop; the `mouse` command drains events on demand) -- a single shared
   queue would let whichever one polls first silently steal the other's
   event, which is exactly what broke the `mouse` command's own
   already-tested contract (test_mouse_selftest.sh) during this
   milestone's design before being caught. Same fixed-capacity/drop-if-
   full contract as the debug queue. */
bool mouse_has_cursor_event(void);
mouse_event_t mouse_get_cursor_event(void);

#endif /* KERNEL_DRIVERS_MOUSE_H */
