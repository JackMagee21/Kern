#include <stdint.h>

#include "heap_alloc.h"

#define ALIGNMENT 16u
#define ALIGN_UP(x) (((x) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))

void heap_alloc_init(heap_alloc_t *heap, void *region, size_t region_size)
{
    heap_block_t *block = (heap_block_t *)region;
    block->size = region_size - sizeof(heap_block_t);
    block->free = 1;
    block->next = NULL;
    heap->free_list = block;
}

void *heap_alloc_alloc(heap_alloc_t *heap, size_t size)
{
    size = ALIGN_UP(size);

    for (heap_block_t *block = heap->free_list; block != NULL; block = block->next) {
        if (!block->free || block->size < size) {
            continue;
        }

        /* Split off the remainder only if it can hold a header plus at
           least one alignment unit of payload -- otherwise the leftover
           sliver would be unusable and just wastes a header's worth of
           bookkeeping. */
        if (block->size >= size + sizeof(heap_block_t) + ALIGNMENT) {
            heap_block_t *remainder = (heap_block_t *)((uint8_t *)(block + 1) + size);
            remainder->size = block->size - size - sizeof(heap_block_t);
            remainder->free = 1;
            remainder->next = block->next;

            block->size = size;
            block->next = remainder;
        }

        block->free = 0;
        return (void *)(block + 1);
    }

    return NULL;
}

void heap_alloc_free(heap_alloc_t *heap, void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    heap_block_t *block = (heap_block_t *)ptr - 1;
    block->free = 1;

    /* Coalesce adjacent free blocks. Singly-linked (no prev pointer),
       so this walks from the head; heap sizes here are small enough
       that O(n) per free is fine. Re-check the same block after a
       merge instead of advancing, so a run of 3+ adjacent free blocks
       fully collapses in one free_list pass. */
    heap_block_t *b = heap->free_list;
    while (b != NULL && b->next != NULL) {
        uint8_t *end_of_b = (uint8_t *)(b + 1) + b->size;
        if (b->free && b->next->free && end_of_b == (uint8_t *)b->next) {
            b->size += sizeof(heap_block_t) + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
}
