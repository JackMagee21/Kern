#include <stddef.h>
#include <stdint.h>

#include "multiboot2.h"

const multiboot2_tag_t *multiboot2_find_tag(uint32_t mbi_addr, uint32_t type)
{
    const multiboot2_info_header_t *info = (const multiboot2_info_header_t *)(uintptr_t)mbi_addr;
    uint32_t total_size = info->total_size;

    const uint8_t *tag_ptr = (const uint8_t *)(uintptr_t)mbi_addr + sizeof(multiboot2_info_header_t);
    const uint8_t *info_end = (const uint8_t *)(uintptr_t)mbi_addr + total_size;

    while (tag_ptr + sizeof(multiboot2_tag_t) <= info_end) {
        const multiboot2_tag_t *tag = (const multiboot2_tag_t *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }
        if (tag->type == type) {
            return tag;
        }

        uint32_t advance = (tag->size + 7) & ~7u; /* tags are 8-byte aligned */
        if (advance == 0) {
            break; /* malformed zero-size tag: stop instead of looping forever */
        }
        tag_ptr += advance;
    }

    return NULL;
}
