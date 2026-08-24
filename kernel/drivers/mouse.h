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

#endif /* KERNEL_DRIVERS_MOUSE_H */
