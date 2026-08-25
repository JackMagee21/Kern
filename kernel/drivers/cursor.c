#include <stdint.h>

#include "cursor.h"
#include "framebuffer.h"
#include "mouse.h"

#define CURSOR_SIZE 8u

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t cursor_color;
static uint32_t saved_pixels[CURSOR_SIZE * CURSOR_SIZE];
static int cursor_drawn;

static void erase_at_current(void)
{
    if (!cursor_drawn) {
        return;
    }
    for (uint32_t y = 0; y < CURSOR_SIZE; y++) {
        for (uint32_t x = 0; x < CURSOR_SIZE; x++) {
            fb_put_pixel(cursor_x + x, cursor_y + y, saved_pixels[y * CURSOR_SIZE + x]);
        }
    }
}

static void draw_at_current(void)
{
    fb_read_rect(cursor_x, cursor_y, CURSOR_SIZE, CURSOR_SIZE, saved_pixels);
    fb_fill_rect(cursor_x, cursor_y, CURSOR_SIZE, CURSOR_SIZE, cursor_color);
    cursor_drawn = 1;
}

void cursor_init(void)
{
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    cursor_x = (fb_w > CURSOR_SIZE) ? (fb_w / 2) : 0;
    cursor_y = (fb_h > CURSOR_SIZE) ? (fb_h / 2) : 0;
    cursor_color = fb_pack_color(0xff, 0x00, 0x00); /* bright red -- unmistakable against console text */
    draw_at_current();
}

void cursor_poll(void)
{
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    /* Both are guaranteed >= CURSOR_SIZE in practice (the negotiated
       1024x768 mode is far larger), but guard the clamp arithmetic
       below against an unexpectedly tiny mode anyway rather than
       underflow. */
    uint32_t max_x = (fb_w > CURSOR_SIZE) ? (fb_w - CURSOR_SIZE) : 0;
    uint32_t max_y = (fb_h > CURSOR_SIZE) ? (fb_h - CURSOR_SIZE) : 0;

    int32_t new_x = (int32_t)cursor_x;
    int32_t new_y = (int32_t)cursor_y;
    int moved = 0;

    while (mouse_has_cursor_event()) {
        mouse_event_t event = mouse_get_cursor_event();
        new_x += event.dx;
        /* PS/2 convention: positive dy = UP (mouse.h's own doc comment)
           -- screen Y grows DOWNWARD, so this is the one place that
           doc comment said a real display consumer would decide its own
           sign convention. Negated here, nowhere else. */
        new_y -= event.dy;
        moved = 1;
    }
    if (!moved) {
        return;
    }

    if (new_x < 0) {
        new_x = 0;
    }
    if (new_y < 0) {
        new_y = 0;
    }
    if ((uint32_t)new_x > max_x) {
        new_x = (int32_t)max_x;
    }
    if ((uint32_t)new_y > max_y) {
        new_y = (int32_t)max_y;
    }

    erase_at_current();
    cursor_x = (uint32_t)new_x;
    cursor_y = (uint32_t)new_y;
    draw_at_current();
}
