#ifndef KERNEL_DRIVERS_FBCONSOLE_H
#define KERNEL_DRIVERS_FBCONSOLE_H

/* Milestone 23 (ADR 0023): the text console, now rendered as 8x8 glyphs
   (kernel/drivers/font8x8.h) onto the linear graphics framebuffer
   (kernel/drivers/framebuffer.h) instead of Milestone 8's legacy VGA
   text-mode buffer (kernel/drivers/vga.c, retired this milestone --
   text mode and a linear framebuffer can't be active simultaneously,
   since GRUB switches the actual video hardware mode to satisfy the
   Multiboot2 framebuffer request tag). Same putc/clear/write interface
   shape vga.c had, so kernel/drivers/console.c's fan-out needed only a
   one-line swap, not a redesign.

   MUST run after fb_init() -- the cell grid's own dimensions
   (fb_get_width()/height() divided by the 8x8 glyph size) aren't known
   until the negotiated framebuffer mode is. Calling fbconsole_putc()
   before fbconsole_init() (or before fb_init(), transitively) is safe
   but a silent no-op -- see framebuffer.c's fb_ready guard doc comment
   for why that's the deliberate choice, not an oversight: a panic/fault
   dump that fires before this point in boot must never itself crash by
   touching an uninitialized console. */
void fbconsole_init(void);
void fbconsole_clear(void);
void fbconsole_putc(char c);
void fbconsole_write(const char *s);

#endif /* KERNEL_DRIVERS_FBCONSOLE_H */
