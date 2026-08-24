#ifndef KERNEL_DRIVERS_VGA_H
#define KERNEL_DRIVERS_VGA_H

/*
 * Legacy VGA text-mode console (memory-mapped buffer at physical
 * 0xB8000, identity-mapped by boot.asm's low 8MiB window -- no new
 * mapping needed). Reliable when booted via legacy BIOS/CSM; on a pure
 * UEFI boot with no CSM, 0xB8000 generally isn't a live VGA text buffer
 * (there's no BIOS video service to have set one up) -- a known,
 * documented limitation, not a bug. See ADR 0008: a Multiboot2
 * framebuffer + bitmap font would work uniformly on both, but is
 * substantially more machinery (font table, pixel plotting, scrolling)
 * for a first console.
 */
void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *s);

#endif /* KERNEL_DRIVERS_VGA_H */
