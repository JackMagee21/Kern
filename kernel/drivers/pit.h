#ifndef KERNEL_DRIVERS_PIT_H
#define KERNEL_DRIVERS_PIT_H

#include <stdint.h>

/* Programs PIT channel 0 for a periodic (mode 3, square wave) interrupt
   at frequency_hz on IRQ0, and registers its own IRQ0 handler. Does NOT
   unmask IRQ0 or enable interrupts -- callers do that once they're
   actually ready to start receiving ticks (pic_clear_mask(0); sti). */
void pit_init(uint32_t frequency_hz);

/* Ticks received so far. Safe to poll from normal (non-interrupt)
   context without disabling interrupts: single-writer (the IRQ0
   handler, which can't itself be reentered while running -- interrupt
   gates clear IF), single simple-load reader, and aligned uint64_t
   reads/writes are atomic on x86_64 -- CLAUDE.md's "justified lock-free
   structure" case, not a race that needs a real lock. */
uint64_t pit_get_ticks(void);

#endif /* KERNEL_DRIVERS_PIT_H */
