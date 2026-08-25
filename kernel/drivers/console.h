#ifndef KERNEL_DRIVERS_CONSOLE_H
#define KERNEL_DRIVERS_CONSOLE_H

#include <stdint.h>

/* Fans every write out to both serial (keeps every existing QEMU smoke
   test working unchanged -- they all grep serial output) and the
   graphics-framebuffer text console (kernel/drivers/fbconsole.c,
   Milestone 23 -- replaced Milestone 8's legacy VGA text-mode console,
   which can't coexist with the linear framebuffer mode GRUB switches
   into to satisfy this kernel's Multiboot2 framebuffer request; see ADR
   0023) so boot self-tests and panics are visible on real hardware with
   no serial cable attached, not just in a virtual machine.
   kernel_main/panic.c/exceptions.c use this instead of calling serial_*
   directly for exactly that reason; drivers.c-level code with no
   user-facing meaning (e.g. internal debug prints, if any existed)
   would have no reason to. Calls made before fbconsole_init() has run
   (early in boot -- see fbconsole.h) still reach serial; the graphics
   half is a silent no-op until then, never a crash. */
void console_putc(char c);
void console_write(const char *s);
void console_write_hex(uint64_t value);

/* Clears the framebuffer console and sends the ANSI clear-screen
   sequence over serial, for the (common) case of watching serial output
   through a real terminal emulator that understands it -- harmless
   no-op sequence of bytes if not. */
void console_clear(void);

#endif /* KERNEL_DRIVERS_CONSOLE_H */
