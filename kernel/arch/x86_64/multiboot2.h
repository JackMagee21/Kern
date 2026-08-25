#ifndef KERNEL_ARCH_X86_64_MULTIBOOT2_H
#define KERNEL_ARCH_X86_64_MULTIBOOT2_H

#include <stdint.h>

/*
 * Multiboot2 info-structure tag layout, verified against the canonical
 * GRUB header (https://raw.githubusercontent.com/rhboot/grub2/master/
 * include/multiboot2.h) -- the same primary source ADR 0001 used for
 * the boot header itself. The 8-byte fixed header (total_size/reserved)
 * that precedes the tag list isn't defined in that C header (GRUB
 * builds it inline, bootloader-side) but is standard Multiboot2 spec
 * structure, not a guess.
 */

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t reserved;
} multiboot2_info_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} multiboot2_tag_t;

#define MULTIBOOT2_TAG_TYPE_END         0
#define MULTIBOOT2_TAG_TYPE_MMAP        6
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries follow, entry_size bytes each */
} multiboot2_tag_mmap_t;

typedef struct __attribute__((packed)) {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} multiboot2_mmap_entry_t;

#define MULTIBOOT2_MEMORY_AVAILABLE 1

/* Milestone 23 (ADR 0023): boot-info framebuffer tag, verified against
   the canonical GRUB header (rhboot/grub2's multiboot2.h,
   struct multiboot_tag_framebuffer_common plus the RGB-specific fields
   that follow it in struct multiboot_tag_framebuffer's union) -- same
   primary source ADR 0001 already used for this header's other
   structures. Only the RGB-color-model member of that union is defined
   here (framebuffer_red/green/blue_field_position/mask_size); the
   INDEXED-color member (a palette) is never used by this codebase --
   kernel/drivers/framebuffer.c panics if framebuffer_type isn't
   MULTIBOOT2_FRAMEBUFFER_TYPE_RGB, so those fields are never read as
   anything but padding in that case. */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
    uint8_t  framebuffer_red_field_position;
    uint8_t  framebuffer_red_mask_size;
    uint8_t  framebuffer_green_field_position;
    uint8_t  framebuffer_green_mask_size;
    uint8_t  framebuffer_blue_field_position;
    uint8_t  framebuffer_blue_mask_size;
} multiboot2_tag_framebuffer_t;

#define MULTIBOOT2_FRAMEBUFFER_TYPE_INDEXED  0
#define MULTIBOOT2_FRAMEBUFFER_TYPE_RGB      1
#define MULTIBOOT2_FRAMEBUFFER_TYPE_EGA_TEXT 2

/* Milestone 23 (ADR 0023): the ONE piece of tag-list-walking logic every
   consumer (pmm.c's mmap parse, framebuffer.c's own init) needs --
   factored out once a second real caller existed (this codebase's
   established "reuse once genuinely duplicated, not preemptively"
   discipline). Returns a pointer to the first tag of the given type in
   mbi_addr's tag list, or NULL if none exists (either the bootloader
   never provided one -- a real, expected outcome for an OPTIONAL
   request tag like the framebuffer one -- or the list is malformed).
   Stops at a zero-size tag rather than looping forever (same defensive
   guard pmm.c's own walk already had). */
const multiboot2_tag_t *multiboot2_find_tag(uint32_t mbi_addr, uint32_t type);

#endif /* KERNEL_ARCH_X86_64_MULTIBOOT2_H */
