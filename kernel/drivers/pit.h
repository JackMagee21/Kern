#ifndef KERNEL_DRIVERS_PIT_H
#define KERNEL_DRIVERS_PIT_H

#include <stdint.h>

/* Programs PIT channel 0 for a periodic (mode 3, square wave) interrupt
   at frequency_hz on IRQ0. Does NOT register an IRQ0 handler, unmask
   IRQ0, or enable interrupts -- purely hardware programming. Since
   Milestone 6, whatever owns IRQ0 (kernel/sched/scheduler.c) is
   responsible for calling pit_tick() itself and doing the rest
   (irq_register_handler, pic_clear_mask, sti) -- keeps this file a
   plain hardware driver with no scheduling-policy dependency. */
void pit_init(uint32_t frequency_hz);

/* Records one tick. Called by whatever owns IRQ0. */
void pit_tick(void);

/* Ticks received so far. Safe to poll from normal (non-interrupt)
   context without disabling interrupts: single-writer (pit_tick,
   called from an interrupt gate context that can't itself be
   reentered while running), single simple-load reader, and aligned
   uint64_t reads/writes are atomic on x86_64 -- CLAUDE.md's "justified
   lock-free structure" case, not a race that needs a real lock. */
uint64_t pit_get_ticks(void);

#endif /* KERNEL_DRIVERS_PIT_H */
