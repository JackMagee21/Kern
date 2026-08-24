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

#define MULTIBOOT2_TAG_TYPE_END  0
#define MULTIBOOT2_TAG_TYPE_MMAP 6

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

#endif /* KERNEL_ARCH_X86_64_MULTIBOOT2_H */
