#ifndef KERNEL_DRIVERS_FRAMEBUFFER_H
#define KERNEL_DRIVERS_FRAMEBUFFER_H

#include <stdint.h>

/* Milestone 23 (ADR 0023): a linear graphics framebuffer, negotiated at
   boot via a Multiboot2 framebuffer request tag (kernel/arch/x86_64/
   boot.asm) and read back from the boot-info structure GRUB fills in
   (kernel/arch/x86_64/multiboot2.h's multiboot2_tag_framebuffer_t) --
   NEVER hardcoded, since the bootloader is free to substitute its own
   best-match mode for the 1024x768x32 requested. Panics if no
   framebuffer tag comes back, or if the negotiated mode isn't
   direct-color RGB (MULTIBOOT2_FRAMEBUFFER_TYPE_RGB) -- an
   INDEXED-palette or EGA_TEXT mode would need entirely different
   pixel-composition logic this driver doesn't implement, the same
   "unsupported hardware configuration -> panic with a clear reason"
   stance vmm_enable_nx() already takes for a CPU lacking NX.

   Physical framebuffer memory is reached via the EXISTING Milestone 19
   direct-map (vmm_phys_to_virt(), kernel/mm/vmm.c) rather than a new
   dedicated mapping -- QEMU's default machine places its VGA/VBE linear
   framebuffer BAR well under 4GiB (observed: 0xfd000000), already
   covered by the direct-map's unconditional 0..4GiB 2MiB-page mapping,
   so no new page-table work is needed for this milestone at all. */
void fb_init(uint32_t mbi_addr);

uint32_t fb_get_width(void);
uint32_t fb_get_height(void);

/* Packs 8-bit r/g/b channel values into this framebuffer's own native
   pixel encoding (bit positions/mask sizes read from the negotiated
   mode's own tag fields, never assumed to be a fixed RGBA/BGRA layout)
   -- callers must go through this rather than hand-building a pixel
   value, so code stays correct regardless of which specific direct-color
   layout the bootloader/hardware actually negotiated. */
uint32_t fb_pack_color(uint8_t r, uint8_t g, uint8_t b);

/* Bounds-checked: silently does nothing if (x, y) falls outside the
   negotiated framebuffer -- matches this codebase's established
   "clamp/no-op at a boundary rather than fault or corrupt adjacent
   memory" stance for anything driven by external input (mouse deltas,
   in this driver's actual callers). */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);

/* Fills the rectangle [x, x+w) x [y, y+h) with color, clipped to the
   framebuffer's own bounds (a request that runs off the edge is
   truncated, not rejected or faulted). */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

/* Copies pixels FROM the framebuffer INTO a caller-provided buffer
   (row-major, w*h uint32_t entries) -- the read half of the sprite
   save/restore pattern kernel/drivers/cursor.c uses to move the mouse
   cursor without leaving a trail or permanently overwriting whatever
   text was underneath it. Clipped the same way fb_fill_rect() is: a
   request that runs off the edge only fills the portion of `out` that
   corresponds to visible pixels, leaving the rest of `out` untouched
   (callers that always request the same size from the same clamped
   on-screen position never actually hit this edge in practice, since
   cursor.c never lets its own footprint run off-screen -- see its own
   doc comment). */
void fb_read_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t *out);

/* Shifts pixel content up by `rows` pixel rows WITHIN [0, region_height)
   only (a raw row-by-row memory copy via the internal base pointer, not
   composed of many bounds-checked single-pixel writes) -- everything at
   y >= region_height is left COMPLETELY untouched -- then fills the
   newly-exposed rows at the bottom of the region with fill_color. The
   pixel-level primitive kernel/drivers/fbconsole.c's own text
   scrolling is built on, kept here rather than exposed via
   fb_put_pixel() in a loop since only this file's internals know the
   raw row stride (fb_pitch may include padding beyond width*4).
   region_height is clamped to fb_height if it's ever larger.

   Milestone 30 (ADR 0030): region_height was added -- previously this
   always shifted the FULL framebuffer, which corrupted any
   already-composited content sitting below the console's own text
   (kernel/user/display_server.c's windows, discovered as a real,
   growing architectural gap in Milestone 28, ADR 0028's own Known
   limitations) once enough console output accumulated to scroll.
   fbconsole.c now reserves a FIXED height for its own scrollable text
   region (well short of the full screen) and passes it here, so
   anything placed at a y at or beyond that reserved height -- the
   "desktop" area -- is now permanently immune to console scrolling,
   not just less likely to be disturbed. */
void fb_scroll_up(uint32_t rows, uint32_t fill_color, uint32_t region_height);

#endif /* KERNEL_DRIVERS_FRAMEBUFFER_H */
