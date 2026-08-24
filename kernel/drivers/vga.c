#include <stdint.h>

#include "vga.h"
#include "../../libk/io.h"

/*
 * Standard VGA text-mode buffer: 80x25 cells, each 2 bytes (low byte
 * ASCII, high byte attribute: bits 0-3 foreground, bits 4-6 background,
 * bit 7 blink). Cursor position ports (CRTC index/data 0x3D4/0x3D5,
 * cursor location high/low registers 0x0E/0x0F) are long-stable, widely
 * documented VGA hardware -- same confidence tier as the PIT/PIC facts
 * already used, not a bit-exact structure whose error mode is a silent
 * triple fault (unlike GDT/IDT/page tables), so no primary-source fetch
 * for this one.
 */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_BUFFER ((volatile uint16_t *)0xb8000)
#define VGA_DEFAULT_ATTR 0x07 /* light grey on black */

/* 0xB8000 is reachable from every address space, not just the kernel's
   own: vmm_create_address_space() (kernel/mm/vmm.c) shares the kernel's
   identity map (PML4[0], supervisor-only) into every process's page
   table alongside the kernel-half (PML4[511]) it already shared, so
   kernel code running under a process's CR3 -- a syscall or exception
   handler, since neither SYSCALL nor an interrupt switches CR3 on
   entry -- can still reach identity-mapped kernel structures like this
   one. See ADR 0009 for how this was found (sys_write's console_putc()
   faulted immediately when called from a running process) and why
   sharing PML4[0] is safe (still supervisor-only, so ring-3 code itself
   still can't touch it). */

#define VGA_CRTC_INDEX       0x3d4
#define VGA_CRTC_DATA        0x3d5
#define VGA_CRTC_CURSOR_HIGH 0x0e
#define VGA_CRTC_CURSOR_LOW  0x0f

static uint8_t cursor_row;
static uint8_t cursor_col;

static void update_hardware_cursor(void)
{
    uint16_t pos = (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);
    outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_HIGH);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xff));
    outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_LOW);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xff));
}

static void clear_row(int row)
{
    for (int col = 0; col < VGA_WIDTH; col++) {
        VGA_BUFFER[row * VGA_WIDTH + col] = (uint16_t)(' ' | (VGA_DEFAULT_ATTR << 8));
    }
}

static void scroll(void)
{
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            VGA_BUFFER[(row - 1) * VGA_WIDTH + col] = VGA_BUFFER[row * VGA_WIDTH + col];
        }
    }
    clear_row(VGA_HEIGHT - 1);
}

void vga_clear(void)
{
    for (int row = 0; row < VGA_HEIGHT; row++) {
        clear_row(row);
    }
    cursor_row = 0;
    cursor_col = 0;
    update_hardware_cursor();
}

void vga_init(void)
{
    vga_clear();
}

void vga_putc(char c)
{
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = (uint16_t)(' ' | (VGA_DEFAULT_ATTR << 8));
        }
    } else {
        VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = (uint16_t)((uint8_t)c | (VGA_DEFAULT_ATTR << 8));
        cursor_col++;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }

    if (cursor_row >= VGA_HEIGHT) {
        scroll();
        cursor_row = VGA_HEIGHT - 1;
    }

    update_hardware_cursor();
}

void vga_write(const char *s)
{
    while (*s != '\0') {
        vga_putc(*s);
        s++;
    }
}
