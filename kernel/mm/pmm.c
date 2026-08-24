#include <stdint.h>

#include "pmm.h"
#include "../arch/x86_64/multiboot2.h"

/*
 * Bitmap allocator, one bit per 4KiB frame. Fixed-size (4GiB worth of
 * bits = 128KiB) static array rather than sized dynamically from the
 * detected memory map, because there is no heap yet to size it into
 * (that's Milestone 4) -- see ADR 0003 for why this is an accepted,
 * documented limit rather than an oversight. Frames beyond
 * PMM_MAX_FRAMES are simply never tracked/allocatable.
 */
#define PMM_MAX_PHYS_BYTES (4ULL * 1024 * 1024 * 1024)
#define PMM_MAX_FRAMES     (PMM_MAX_PHYS_BYTES / PMM_FRAME_SIZE)
#define PMM_BITMAP_BYTES   (PMM_MAX_FRAMES / 8)

static uint8_t frame_bitmap[PMM_BITMAP_BYTES]; /* zeroed by boot.asm's .bss clear */
static uint64_t frames_free_count;

/* Milestone 21 (ADR 0021, copy-on-write fork): one entry per frame,
   same PMM_MAX_FRAMES sizing convention frame_bitmap already uses (a
   flat array indexed by frame number, not a sparse structure -- simple
   and consistent, and this kernel already accepts a fixed 4GiB-worth
   bookkeeping cost for the bitmap, so one more byte-per-frame array is
   the same kind of tradeoff, not a new one). 0 for a never-allocated or
   currently-free frame; 1 the instant pmm_alloc_frame() hands it out
   (its single implicit owner); only ever > 1 once pmm_frame_addref()
   has been called on it at least once (COW-sharing a page across a
   fork). uint16_t, not uint8_t: cheap headroom (2MiB total instead of
   1MiB) against a frame being fork-shared by dozens of descendants
   without silently wrapping -- still finite (65535), an accepted limit
   for a hobby kernel with no realistic path to that many concurrent
   forks of the same page. */
static uint16_t frame_refcount[PMM_MAX_FRAMES]; /* zeroed by boot.asm's .bss clear */

extern char kernel_end_lma[]; /* boot/linker.ld: physical end of the kernel image */

static inline int bitmap_test(uint64_t frame)
{
    return (frame_bitmap[frame / 8] >> (frame % 8)) & 1;
}

static inline void bitmap_set(uint64_t frame)
{
    frame_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void bitmap_clear(uint64_t frame)
{
    frame_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static void mark_used_range(uint64_t start_addr, uint64_t end_addr_exclusive)
{
    uint64_t start_frame = start_addr / PMM_FRAME_SIZE;
    uint64_t end_frame = (end_addr_exclusive + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (end_frame > PMM_MAX_FRAMES) {
        end_frame = PMM_MAX_FRAMES;
    }
    for (uint64_t f = start_frame; f < end_frame; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            frames_free_count--;
        }
    }
}

static void mark_free_range(uint64_t start_addr, uint64_t end_addr_exclusive)
{
    uint64_t start_frame = start_addr / PMM_FRAME_SIZE;
    if (start_addr % PMM_FRAME_SIZE != 0) {
        start_frame++; /* never free a partial leading frame */
    }
    uint64_t end_frame = end_addr_exclusive / PMM_FRAME_SIZE; /* round down: never free a partial trailing frame */
    if (end_frame > PMM_MAX_FRAMES) {
        end_frame = PMM_MAX_FRAMES;
    }
    for (uint64_t f = start_frame; f < end_frame; f++) {
        if (bitmap_test(f)) {
            bitmap_clear(f);
            frames_free_count++;
        }
    }
}

void pmm_init(uint32_t mbi_addr)
{
    /* Default deny: every frame starts used until the memory map proves
       it available. Safer default for a parser reading bootloader-
       supplied data -- a map we fail to parse just means less usable
       memory, never a frame handed out that shouldn't be. */
    for (uint64_t i = 0; i < PMM_BITMAP_BYTES; i++) {
        frame_bitmap[i] = 0xff;
    }
    frames_free_count = 0;

    const multiboot2_info_header_t *info = (const multiboot2_info_header_t *)(uintptr_t)mbi_addr;
    uint32_t total_size = info->total_size;

    const uint8_t *tag_ptr = (const uint8_t *)(uintptr_t)mbi_addr + sizeof(multiboot2_info_header_t);
    const uint8_t *info_end = (const uint8_t *)(uintptr_t)mbi_addr + total_size;

    while (tag_ptr + sizeof(multiboot2_tag_t) <= info_end) {
        const multiboot2_tag_t *tag = (const multiboot2_tag_t *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP && tag->size >= sizeof(multiboot2_tag_mmap_t)) {
            const multiboot2_tag_mmap_t *mmap = (const multiboot2_tag_mmap_t *)tag_ptr;
            if (mmap->entry_size != 0) {
                uint32_t entry_count = (mmap->size - sizeof(multiboot2_tag_mmap_t)) / mmap->entry_size;
                const uint8_t *entry_ptr = tag_ptr + sizeof(multiboot2_tag_mmap_t);
                for (uint32_t i = 0; i < entry_count; i++) {
                    const multiboot2_mmap_entry_t *entry = (const multiboot2_mmap_entry_t *)entry_ptr;
                    if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE) {
                        mark_free_range(entry->base_addr, entry->base_addr + entry->length);
                    }
                    entry_ptr += mmap->entry_size;
                }
            }
        }

        uint32_t advance = (tag->size + 7) & ~7u; /* tags are 8-byte aligned */
        if (advance == 0) {
            break; /* malformed zero-size tag: stop instead of looping forever */
        }
        tag_ptr += advance;
    }

    /* Reserve what must never be handed out: physical address 0 (so an
       allocation is never confused with a null pointer), everything
       from 0 through the end of the kernel image (covers the sub-1MiB
       legacy BIOS region, boot page tables/GDT/stack, and the kernel's
       own code/data/bss), and the multiboot info structure this
       function just read. The memory map above may have marked all of
       this "available" too -- GRUB has no idea a kernel is sitting on
       it -- so this must run after, not before, the loop above. */
    mark_used_range(0, (uint64_t)(uintptr_t)kernel_end_lma);
    mark_used_range(mbi_addr, (uint64_t)mbi_addr + total_size);
}

uint64_t pmm_alloc_frame(void)
{
    for (uint64_t frame = 1; frame < PMM_MAX_FRAMES; frame++) { /* frame 0 is permanently reserved */
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            frames_free_count--;
            frame_refcount[frame] = 1; /* the caller is this frame's single implicit owner */
            return frame * PMM_FRAME_SIZE;
        }
    }
    return 0;
}

void pmm_frame_addref(uint64_t phys_addr)
{
    if (phys_addr == 0 || phys_addr % PMM_FRAME_SIZE != 0) {
        return;
    }
    uint64_t frame = phys_addr / PMM_FRAME_SIZE;
    if (frame >= PMM_MAX_FRAMES || !bitmap_test(frame)) {
        return; /* not currently allocated -- misuse, not a real reference to add */
    }
    frame_refcount[frame]++;
}

uint32_t pmm_frame_refcount(uint64_t phys_addr)
{
    if (phys_addr == 0 || phys_addr % PMM_FRAME_SIZE != 0) {
        return 0;
    }
    uint64_t frame = phys_addr / PMM_FRAME_SIZE;
    if (frame >= PMM_MAX_FRAMES || !bitmap_test(frame)) {
        return 0;
    }
    return frame_refcount[frame];
}

void pmm_free_frame(uint64_t phys_addr)
{
    if (phys_addr == 0 || phys_addr % PMM_FRAME_SIZE != 0) {
        return;
    }
    uint64_t frame = phys_addr / PMM_FRAME_SIZE;
    if (frame >= PMM_MAX_FRAMES) {
        return;
    }
    if (!bitmap_test(frame)) {
        return;
    }
    if (frame_refcount[frame] > 1) {
        frame_refcount[frame]--; /* still referenced elsewhere -- not actually free yet */
        return;
    }
    frame_refcount[frame] = 0;
    bitmap_clear(frame);
    frames_free_count++;
}

uint64_t pmm_frames_free(void)
{
    return frames_free_count;
}

uint64_t pmm_frames_total(void)
{
    return PMM_MAX_FRAMES;
}
