#include <stdint.h>

#include "fbconsole.h"
#include "framebuffer.h"
#include "font8x8.h"

#define GLYPH_W 8u
#define GLYPH_H 8u

/* Colors matching vga.c's own VGA_DEFAULT_ATTR (0x07, light grey on
   black) as closely as an 8-bit-per-channel RGB framebuffer can. */
static uint32_t fg_color;
static uint32_t bg_color;

static uint32_t cols; /* 0 until fbconsole_init() runs -- also this module's "not ready" sentinel */
static uint32_t rows;
static uint32_t cursor_col;
static uint32_t cursor_row;

static void draw_glyph(uint32_t col, uint32_t row, char c)
{
    uint8_t uc = (uint8_t)c;
    const uint8_t *glyph = (uc >= 0x20 && uc <= 0x7e) ? font8x8_basic[uc - 0x20] : font8x8_basic[0];

    uint32_t px = col * GLYPH_W;
    uint32_t py = row * GLYPH_H;
    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            /* bit N = column N counted from the left (LSB = leftmost
               pixel) -- verified visually via QEMU screendump, see ADR
               0023's Verification section, not trusted from memory. */
            int on = (bits >> gx) & 1;
            fb_put_pixel(px + gx, py + gy, on ? fg_color : bg_color);
        }
    }
}

static void scroll(void)
{
    fb_scroll_up(GLYPH_H, bg_color);
}

void fbconsole_init(void)
{
    cols = fb_get_width() / GLYPH_W;
    rows = fb_get_height() / GLYPH_H;
    fg_color = fb_pack_color(0xc0, 0xc0, 0xc0);
    bg_color = fb_pack_color(0x00, 0x00, 0x00);
    fbconsole_clear();
}

void fbconsole_clear(void)
{
    if (cols == 0) {
        return; /* not initialized yet -- see this file's header doc comment */
    }
    fb_fill_rect(0, 0, cols * GLYPH_W, rows * GLYPH_H, bg_color);
    cursor_row = 0;
    cursor_col = 0;
}

void fbconsole_putc(char c)
{
    if (cols == 0) {
        return; /* not initialized yet -- see this file's header doc comment */
    }

    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            draw_glyph(cursor_col, cursor_row, ' ');
        }
    } else {
        draw_glyph(cursor_col, cursor_row, c);
        cursor_col++;
        if (cursor_col >= cols) {
            cursor_col = 0;
            cursor_row++;
        }
    }

    if (cursor_row >= rows) {
        scroll();
        cursor_row = rows - 1;
    }
}

void fbconsole_write(const char *s)
{
    while (*s != '\0') {
        fbconsole_putc(*s);
        s++;
    }
}
