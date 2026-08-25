#include <stddef.h>
#include <stdint.h>

#include "framebuffer.h"
#include "../arch/x86_64/multiboot2.h"
#include "../mm/vmm.h"
#include "../panic.h"

static uint8_t *fb_base;      /* byte pointer, via vmm_phys_to_virt() */
static uint32_t fb_pitch;     /* bytes per row -- NOT necessarily width*4, see doc comment */
static uint32_t fb_width;
static uint32_t fb_height;
static uint8_t  fb_red_pos, fb_red_size;
static uint8_t  fb_green_pos, fb_green_size;
static uint8_t  fb_blue_pos, fb_blue_size;

/* Milestone 23 (ADR 0023): fb_init() can only run once vmm_phys_to_virt()
   is usable (Milestone 19's direct-map, itself only available partway
   through kernel_main -- after pmm_init()) -- meaningfully LATER than
   every earlier boot message. Every fb_* mutator below checks this
   FIRST and silently no-ops if it's still false, so a panic/fault dump
   that happens to fire before fb_init() has run (or console_putc()
   being called at all before then) can never crash by touching an
   uninitialized fb_base -- CLAUDE.md's "never fail silently on the
   panic path" cuts the other way here: a panic path that itself faults
   would hide the real error, which is worse than the framebuffer half
   of this boot's output starting a few lines later than serial's. */
static int fb_ready;

void fb_init(uint32_t mbi_addr)
{
    const multiboot2_tag_framebuffer_t *tag =
        (const multiboot2_tag_framebuffer_t *)multiboot2_find_tag(mbi_addr, MULTIBOOT2_TAG_TYPE_FRAMEBUFFER);
    if (tag == NULL) {
        panic("fb_init: no Multiboot2 framebuffer tag -- bootloader did not honor the request (boot.asm)");
    }
    if (tag->framebuffer_type != MULTIBOOT2_FRAMEBUFFER_TYPE_RGB) {
        panic("fb_init: negotiated framebuffer is not direct-color RGB (unsupported mode)");
    }
    if (tag->framebuffer_bpp != 32) {
        panic("fb_init: negotiated framebuffer is not 32 bits per pixel (unsupported depth)");
    }

    fb_pitch = tag->framebuffer_pitch;
    fb_width = tag->framebuffer_width;
    fb_height = tag->framebuffer_height;
    fb_red_pos = tag->framebuffer_red_field_position;
    fb_red_size = tag->framebuffer_red_mask_size;
    fb_green_pos = tag->framebuffer_green_field_position;
    fb_green_size = tag->framebuffer_green_mask_size;
    fb_blue_pos = tag->framebuffer_blue_field_position;
    fb_blue_size = tag->framebuffer_blue_mask_size;

    /* Milestone 19's direct-map already covers physical 0..4GiB
       unconditionally (vmm_direct_map_init(), called earlier in
       kernel_main) -- QEMU's default machine places its framebuffer BAR
       well under that (observed 0xfd000000), so no new mapping is
       needed. Defensive check anyway: this driver has no fallback for a
       framebuffer address the direct-map doesn't cover, and a silent
       wraparound/garbage pointer would be far worse than a clear panic. */
    if (tag->framebuffer_addr >= 0x100000000ULL) {
        panic("fb_init: framebuffer physical address is outside the 4GiB direct-map window");
    }
    fb_base = (uint8_t *)(uintptr_t)vmm_phys_to_virt(tag->framebuffer_addr);
    fb_ready = 1; /* every field above is now valid -- set last, deliberately */
}

uint32_t fb_get_width(void)
{
    return fb_width;
}

uint32_t fb_get_height(void)
{
    return fb_height;
}

static inline uint32_t channel_to_field(uint8_t value, uint8_t pos, uint8_t size)
{
    /* size is typically 8 (one byte per channel in a 32bpp mode) but is
       taken from the negotiated tag, not assumed -- a shift-right
       discards low bits if a real mode ever reports fewer, matching how
       every other channel-packing routine of this shape (e.g. classic
       RGB565 conversion) handles a narrower field. */
    uint32_t max = (size >= 8) ? 0xffu : ((1u << size) - 1u);
    uint32_t scaled = (size >= 8) ? value : (uint32_t)value >> (8 - size);
    return (scaled & max) << pos;
}

uint32_t fb_pack_color(uint8_t r, uint8_t g, uint8_t b)
{
    return channel_to_field(r, fb_red_pos, fb_red_size)
         | channel_to_field(g, fb_green_pos, fb_green_size)
         | channel_to_field(b, fb_blue_pos, fb_blue_size);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!fb_ready || x >= fb_width || y >= fb_height) {
        return;
    }
    uint32_t *pixel = (uint32_t *)(fb_base + (uint64_t)y * fb_pitch + (uint64_t)x * 4);
    *pixel = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (!fb_ready) {
        return;
    }
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;
    if (x_end > fb_width) {
        x_end = fb_width;
    }
    if (y_end > fb_height) {
        y_end = fb_height;
    }
    for (uint32_t py = y; py < y_end; py++) {
        for (uint32_t px = x; px < x_end; px++) {
            fb_put_pixel(px, py, color);
        }
    }
}

void fb_read_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t *out)
{
    if (!fb_ready) {
        return;
    }
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;
    if (x_end > fb_width) {
        x_end = fb_width;
    }
    if (y_end > fb_height) {
        y_end = fb_height;
    }
    for (uint32_t py = y; py < y_end; py++) {
        for (uint32_t px = x; px < x_end; px++) {
            const uint32_t *pixel = (const uint32_t *)(fb_base + (uint64_t)py * fb_pitch + (uint64_t)px * 4);
            out[(py - y) * w + (px - x)] = *pixel;
        }
    }
}

void fb_scroll_up(uint32_t rows, uint32_t fill_color)
{
    if (!fb_ready) {
        return;
    }
    if (rows >= fb_height) {
        fb_fill_rect(0, 0, fb_width, fb_height, fill_color);
        return;
    }
    for (uint32_t y = 0; y < fb_height - rows; y++) {
        uint8_t *dst = fb_base + (uint64_t)y * fb_pitch;
        const uint8_t *src = fb_base + (uint64_t)(y + rows) * fb_pitch;
        for (uint32_t b = 0; b < fb_pitch; b++) {
            dst[b] = src[b];
        }
    }
    fb_fill_rect(0, fb_height - rows, fb_width, rows, fill_color);
}
