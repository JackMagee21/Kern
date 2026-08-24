#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

/* Shared by kernel_main and (from Milestone 4 on) kernel/mm/vmm.c and
   heap.c -- print full state to serial, then halt, never fail silently
   or auto-reboot (CLAUDE.md safety rule 6). Not much "state" to print
   yet beyond the message itself; exceptions.c's isr_handler is the one
   that actually dumps registers, for faults that go through the IDT. */
void panic(const char *message) __attribute__((noreturn));

#endif /* KERNEL_PANIC_H */
