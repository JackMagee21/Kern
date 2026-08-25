#include <stdint.h>

#include "fbconsole.h"
#include "framebuffer.h"
#include "font8x8.h"
#include "cursor.h"

#define GLYPH_W 8u
#define GLYPH_H 8u

/* Milestone 30 (ADR 0030): the console's own text/scroll region is
   capped at this many pixel ROWS from the top -- deliberately LESS
   than the full negotiated screen height (Milestone 23's own 1024x768
   request; this leaves 288px, comfortably more than
   kernel/user/display_server.c's own two 150px-tall windows need,
   below it) -- rather than always using fb_get_height() the way this
   driver did before this milestone. Anything drawn at y >= this value
   (kernel/user/display_server.c's own windows, positioned exactly to
   respect this boundary -- see its own doc comment, which references
   this exact constant by value since userspace code can't include
   this header) is now permanently IMMUNE to fb_scroll_up() -- it never
   touches rows outside [0, FBCONSOLE_MAX_HEIGHT_PX) at all, a real fix
   for the drift Milestone 28 found and flagged (ADR 0028's own Known
   limitations), not just a smaller window for it to happen in. A
   negotiated mode shorter than this (unlikely -- Milestone 23 panics
   if the bootloader can't deliver anything at all, but doesn't
   guarantee a MINIMUM size) would just mean fb_scroll_up()'s own
   region_height clamp (framebuffer.c) silently uses the real screen
   height instead -- console text still works, it just has less room,
   same graceful-degradation stance every other "generous, not exact"
   constant in this codebase already takes. */
#define FBCONSOLE_MAX_HEIGHT_PX 480u

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

/* Milestone 28 (ADR 0028): wrapped with cursor_hide()/cursor_show() --
   a real bug, found via test_framebuffer_selftest.sh's own existing
   ghost-trail check once Milestone 28's extra boot-time console output
   pushed this scroll threshold past a point earlier milestones never
   reached. fb_scroll_up() shifts the ENTIRE framebuffer, including
   whatever's under an already-drawn cursor sprite; without hiding it
   first, the cursor's own next erase/redraw (kernel/drivers/cursor.c)
   would restore/composite against pixel data invalidated by this
   shift, corrupting the display. This is fbconsole.c's own only
   coupling to a specific consumer (cursor.c) -- deliberate, not
   layering creep: cursor.c is the one thing besides console text that
   can be sitting on the framebuffer at an arbitrary moment this
   function has no other way to find out about. cursor_hide()/
   cursor_show() are no-ops before cursor_init() has ever run, so this
   is always safe to call unconditionally, even from console output
   that happens before the mouse subsystem exists. */
/* Milestone 30 (ADR 0030): does NOT call cursor_show() itself, unlike
   the version this replaced -- its only caller, fbconsole_putc(),
   already wraps its ENTIRE body in one cursor_hide()/cursor_show()
   pair (below). A show() here too would run BEFORE putc()'s own
   trailing show() with no intervening hide() -- a real bug found and
   fixed this same milestone: the second show() would fb_read_rect()
   the cursor's own just-drawn sprite pixels as "background" (since
   nothing hid it again in between) and cache that as saved_pixels,
   so the NEXT real hide() would "restore" solid cursor color instead
   of the actual background -- a self-reinforcing corruption that
   spread across the whole console region after enough scroll cycles,
   caught via a direct screendump pixel dump, not guessed. cursor_hide()
   here is still correct/needed on its own: it's what makes
   fb_scroll_up() itself operate on a cursor-free framebuffer. */
static void scroll(void)
{
    cursor_hide();
    fb_scroll_up(GLYPH_H, bg_color, FBCONSOLE_MAX_HEIGHT_PX);
}

void fbconsole_init(void)
{
    cols = fb_get_width() / GLYPH_W;
    /* Milestone 30: capped at FBCONSOLE_MAX_HEIGHT_PX, not the full
       fb_get_height() -- see that constant's own doc comment. */
    uint32_t console_height_px = fb_get_height();
    if (console_height_px > FBCONSOLE_MAX_HEIGHT_PX) {
        console_height_px = FBCONSOLE_MAX_HEIGHT_PX;
    }
    rows = console_height_px / GLYPH_H;
    fg_color = fb_pack_color(0xc0, 0xc0, 0xc0);
    bg_color = fb_pack_color(0x00, 0x00, 0x00);
    fbconsole_clear();
}

void fbconsole_clear(void)
{
    if (cols == 0) {
        return; /* not initialized yet -- see this file's header doc comment */
    }
    /* Milestone 30 (ADR 0030): a no-op before cursor_init() has ever
       run (fbconsole_init()'s own call to this function, the only
       caller that predates the cursor) -- but console_clear() (the
       shell's `clear` command) calls this again LATER, once the mouse
       cursor genuinely exists and could be sitting anywhere within the
       cleared region; without hiding it first, this fill would corrupt
       the cursor's own restore buffer the exact same way an unguarded
       fb_scroll_up() call already did (Milestone 28, ADR 0028's Known
       limitations; ADR 0030 fixed the scroll case). Interrupts
       disabled around the whole thing for the same reason
       fbconsole_putc() now is -- the shell's own read_line() loop
       (this function's only OTHER caller) runs with interrupts enabled,
       so without this a preempting ring-3 process's own concurrent
       sys_write() could interleave with this fill and corrupt the
       shared cursor state the identical way Milestone 30's own real
       bug did. */
    unsigned long flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    cursor_hide();
    fb_fill_rect(0, 0, cols * GLYPH_W, rows * GLYPH_H, bg_color);
    cursor_row = 0;
    cursor_col = 0;
    cursor_show();
    if (flags & (1UL << 9)) {
        __asm__ volatile("sti");
    }
}

void fbconsole_putc(char c)
{
    if (cols == 0) {
        return; /* not initialized yet -- see this file's header doc comment */
    }

    /* Milestone 30 (ADR 0030): TWO real bugs, found together via a
       genuine screendump-visible corruption this milestone's own
       bounded-scroll change (more frequent scrolling, from a smaller
       console region) happened to make visible -- neither was actually
       caused by that change, both pre-existed it.

       (1) draw_glyph() below writes directly via fb_put_pixel(), with
       no awareness of the mouse cursor sprite at all -- any character
       whose target column/row overlaps the cursor's OWN current 8x8
       footprint corrupts it. Fixed by wrapping this whole function's
       body in cursor_hide()/cursor_show(), same reasoning as
       fbconsole.c's own scroll()/fbconsole_clear() fixes.

       (2) THE REAL root cause of the corruption actually observed: this
       function -- and the cursor_visible/saved_pixels state machine
       it and cursor_poll() share (kernel/drivers/cursor.c) -- is called
       from `console_putc()`, which BOTH kernel-side code AND
       sys_write() (kernel/arch/x86_64/syscall.c, reachable from EVERY
       ring-3 process) call with interrupts enabled and no
       synchronization at all. Milestones 27-29 all added ring-3
       processes that print several boot markers each -- with enough of
       them now printing around the same time, the preemptive
       scheduler's own timer IRQ can genuinely interrupt ONE process's
       call to this function midway through and run ANOTHER process's
       own call to completion before the first resumes -- two
       INTERLEAVED callers each individually correct in isolation,
       but never mutually exclusive, corrupting cursor_visible/
       saved_pixels (found via direct instrumentation: two
       draw_at_current() calls back to back with no erase_at_current()
       between them, cursor_visible desynchronized from what was
       actually on screen). Fixed by disabling interrupts for this
       function's ENTIRE body (CLAUDE.md: "disable interrupts around a
       critical section... keep it as short as provable" -- bounded to
       one glyph draw plus at most one bounded-region scroll, never
       unbounded), the same save/restore-flags idiom
       kernel/sched/scheduler.c's own scheduler_wake() already
       established (Milestone 25) -- NOT a bare cli/sti, which would
       incorrectly force interrupts ON even when called from code that
       had them deliberately off (e.g. every printf-style call during
       kernel_main's own early boot, before its own sti). Only
       fbconsole's own framebuffer/cursor work needs this protection --
       console_putc()'s OTHER job, serial_putc(), is untouched by this
       race (cursor.c never touches the serial port) and isn't wrapped,
       keeping the disabled window as small as the actual shared state
       requires. */
    unsigned long flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");

    cursor_hide();

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

    cursor_show();

    if (flags & (1UL << 9)) {
        __asm__ volatile("sti");
    }
}

void fbconsole_write(const char *s)
{
    while (*s != '\0') {
        fbconsole_putc(*s);
        s++;
    }
}
