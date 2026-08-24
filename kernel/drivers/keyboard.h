#ifndef KERNEL_DRIVERS_KEYBOARD_H
#define KERNEL_DRIVERS_KEYBOARD_H

#include <stdbool.h>

/* Registers the IRQ1 handler. Does NOT unmask IRQ1 -- callers do that
   once ready (matches the existing pic_clear_mask(0) pattern for the
   timer). */
void keyboard_init(void);

bool keyboard_has_char(void);

/* Returns the next available character, or 0 if none is queued.
   Non-blocking -- callers that want to block poll this in a
   `while (!keyboard_has_char()) hlt;` loop, the same idle-wait pattern
   used everywhere else in this kernel. */
char keyboard_getc(void);

#endif /* KERNEL_DRIVERS_KEYBOARD_H */
