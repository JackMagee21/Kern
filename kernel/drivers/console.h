#ifndef KERNEL_DRIVERS_CONSOLE_H
#define KERNEL_DRIVERS_CONSOLE_H

#include <stdint.h>

/* Fans every write out to both serial (keeps every existing QEMU smoke
   test working unchanged -- they all grep serial output) and the
   graphics-framebuffer text console (kernel/drivers/fbconsole.c,
   Milestone 23 -- replaced Milestone 8's legacy VGA text-mode console,
   which can't coexist with the linear framebuffer mode GRUB switches
   into to satisfy this kernel's Multiboot2 framebuffer request; see ADR
   0023).

   Milestone 37 (ADR 0037) narrowed WHO uses this: genuine unrecoverable
   failures (panic.c, exceptions.c's own fault dump, and
   scheduler.c's flight-recorder dump that accompanies a real #GP) still
   go through console_write/console_write_hex here, so a crash stays
   visible on real hardware with no serial cable attached -- the exact
   reason this fan-out existed in the first place, per CLAUDE.md safety
   rule 6 ("never fail silently") applied to the on-screen experience,
   not just serial. shell.c also still uses these directly -- its own
   prompt/echo/output IS the actual interactive OS surface a person at
   the keyboard is meant to see. Everything else that used to print here
   (kernel_main's own boot self-test markers, the reaper's per-process
   reap line, input_router.c's per-click/drag trace, every ring-3
   process's own sys_write() diagnostic markers) moved to console_log/
   console_log_hex below -- routine, expected, frequent chatter that
   has no reason to clutter a desktop a person is actually looking at,
   but must still reach serial in full, since every existing QEMU smoke
   test greps that log, not the screen. Calls made before
   fbconsole_init() has run (early in boot -- see fbconsole.h) still
   reach serial; the graphics half is a silent no-op until then, never
   a crash. */
void console_putc(char c);
void console_write(const char *s);
void console_write_hex(uint64_t value);

/* Milestone 37 (ADR 0037): serial-only -- see this header's own top
   comment for the exact line between this and console_write/
   console_write_hex above. Never touches fbconsole; safe to call at
   any point in boot, even before fbconsole_init(), the same as
   serial_write() itself always has been. */
void console_log(const char *s);
void console_log_hex(uint64_t value);

/* Clears the framebuffer console and sends the ANSI clear-screen
   sequence over serial, for the (common) case of watching serial output
   through a real terminal emulator that understands it -- harmless
   no-op sequence of bytes if not. */
void console_clear(void);

#endif /* KERNEL_DRIVERS_CONSOLE_H */
