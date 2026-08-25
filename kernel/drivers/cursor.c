#include <stdint.h>

#include "cursor.h"
#include "framebuffer.h"
#include "mouse.h"
#include "input_router.h"

#define CURSOR_SIZE 8u

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t cursor_color;
static uint32_t saved_pixels[CURSOR_SIZE * CURSOR_SIZE];
/* Milestone 28 (ADR 0028): split from the single old "cursor_drawn"
   flag into two, after a real ghost-trail bug found via
   test_framebuffer_selftest.sh once Milestone 28's own extra boot-time
   console output pushed fbconsole.c's scroll() past a threshold it
   hadn't crossed before. cursor_ready: true once cursor_init() has
   run (this driver's own "am I usable at all yet" guard, same
   role the old flag partly played). cursor_visible: true iff the
   sprite is CURRENTLY actually rendered on screen right now -- false
   between a cursor_hide() and the next cursor_show()/cursor_poll()
   redraw. The distinction matters because cursor_hide()/cursor_show()
   (below) can now be called from OUTSIDE this file entirely, wrapped
   around fbconsole.c's own fb_scroll_up() call: erase_at_current()'s
   saved_pixels restore is only ever valid PIXEL DATA nobody else has
   touched since -- fb_scroll_up() shifts the ENTIRE framebuffer,
   including whatever's under an on-screen cursor sprite, so restoring
   stale saved_pixels AFTER a scroll (or drawing a "new" sprite without
   first erasing the OLD one, now sitting at a visually wrong spot)
   corrupts the display exactly the way this bug did -- caught by
   test_framebuffer_selftest.sh's own existing ghost-trail check
   (ADR 0023), not a new one. */
static int cursor_ready;
static int cursor_visible;
/* Milestone 29 (ADR 0029): the PS/2 `left` field is a LEVEL (the
   button's current physical state, per report), not an edge -- this
   driver is the one place that turns it into a genuine click
   transition (false -> true), since it's already the only consumer
   that sees every mouse report's button state alongside the
   authoritative on-screen cursor position a click needs to be
   reported at. */
static int left_was_down;

static void erase_at_current(void)
{
    if (!cursor_visible) {
        return;
    }
    for (uint32_t y = 0; y < CURSOR_SIZE; y++) {
        for (uint32_t x = 0; x < CURSOR_SIZE; x++) {
            fb_put_pixel(cursor_x + x, cursor_y + y, saved_pixels[y * CURSOR_SIZE + x]);
        }
    }
    cursor_visible = 0;
}

static void draw_at_current(void)
{
    fb_read_rect(cursor_x, cursor_y, CURSOR_SIZE, CURSOR_SIZE, saved_pixels);
    fb_fill_rect(cursor_x, cursor_y, CURSOR_SIZE, CURSOR_SIZE, cursor_color);
    cursor_visible = 1;
}

void cursor_init(void)
{
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    cursor_x = (fb_w > CURSOR_SIZE) ? (fb_w / 2) : 0;
    cursor_y = (fb_h > CURSOR_SIZE) ? (fb_h / 2) : 0;
    cursor_color = fb_pack_color(0xff, 0x00, 0x00); /* bright red -- unmistakable against console text */
    cursor_ready = 1;
    draw_at_current();
}

/* Milestone 28 (ADR 0028): erases the cursor sprite (restoring the
   real pixels underneath it) if it's currently visible; a no-op
   before cursor_init() has ever run, or if it's already hidden.
   Callable from OUTSIDE this file -- fbconsole.c's own scroll() is the
   first real caller, wrapping it around fb_scroll_up() so that bulk
   operation never has to reason about a sprite sitting on top of the
   content it's about to shift. */
void cursor_hide(void)
{
    if (!cursor_ready) {
        return;
    }
    erase_at_current();
}

/* Milestone 28 (ADR 0028): redraws the cursor sprite at its CURRENT
   logical (cursor_x, cursor_y) -- unchanged by whatever happened while
   it was hidden -- capturing a FRESH background via fb_read_rect()
   first, so it composites correctly on top of whatever is actually
   there now (e.g. the framebuffer content fb_scroll_up() just
   shifted), not stale pre-hide data. A no-op before cursor_init() has
   ever run. */
void cursor_show(void)
{
    if (!cursor_ready) {
        return;
    }
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
    int click_detected = 0; /* Milestone 29: at most one routed click per poll, even if several packets batched together this round -- see cursor.h's own doc comment */

    while (mouse_has_cursor_event()) {
        mouse_event_t event = mouse_get_cursor_event();
        new_x += event.dx;
        /* PS/2 convention: positive dy = UP (mouse.h's own doc comment)
           -- screen Y grows DOWNWARD, so this is the one place that
           doc comment said a real display consumer would decide its own
           sign convention. Negated here, nowhere else. */
        new_y -= event.dy;
        moved = 1;

        if (event.left && !left_was_down) {
            click_detected = 1;
        }
        left_was_down = event.left;
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

    /* Milestone 29 (ADR 0029): reported AFTER cursor_x/cursor_y are
       finalized (clamped, assigned, and actually redrawn) -- the exact
       position a click is routed at always matches what's visually
       true on screen at that moment, not a possibly-unclamped
       mid-batch value. */
    if (click_detected) {
        input_router_notify_click(cursor_x, cursor_y);
    }
}
