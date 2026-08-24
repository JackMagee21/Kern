#ifndef KERNEL_DRIVERS_CONSOLE_H
#define KERNEL_DRIVERS_CONSOLE_H

#include <stdint.h>

/* Fans every write out to both serial (keeps every existing QEMU smoke
   test working unchanged -- they all grep serial output) and the VGA
   text console (so boot self-tests and panics are visible on real
   hardware with no serial cable attached, not just in a virtual
   machine). kernel_main/panic.c/exceptions.c use this instead of
   calling serial_* directly for exactly that reason; drivers.c-level
   code with no user-facing meaning (e.g. internal debug prints, if any
   existed) would have no reason to. */
void console_putc(char c);
void console_write(const char *s);
void console_write_hex(uint64_t value);

/* Clears the VGA screen and sends the ANSI clear-screen sequence over
   serial, for the (common) case of watching serial output through a
   real terminal emulator that understands it -- harmless no-op
   sequence of bytes if not. */
void console_clear(void);

#endif /* KERNEL_DRIVERS_CONSOLE_H */
