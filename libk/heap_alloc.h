#ifndef LIBK_HEAP_ALLOC_H
#define LIBK_HEAP_ALLOC_H

#include <stddef.h>

/*
 * First-fit free-list allocator over an arbitrary caller-supplied
 * region. Pure logic, no I/O or hardware dependency -- host-testable
 * (tests/host/test_heap_alloc.c) per CLAUDE.md's "factor host-testable
 * logic out of kernel-only code" rule. kernel/mm/heap.c is the thin
 * kmalloc/kfree wrapper that backs a region of this with real page
 * frames via the VMM; this file knows nothing about paging.
 *
 * heap_block_t is this allocator's own bookkeeping format, not a
 * hardware/spec layout (unlike GDT/IDT/page-table entries elsewhere in
 * this kernel) -- ordinary compiler struct layout is fine here, no
 * explicit packing needed.
 */
typedef struct heap_block {
    size_t size; /* payload size in bytes, not including this header */
    int free;
    struct heap_block *next;
} heap_block_t;

typedef struct {
    heap_block_t *free_list;
} heap_alloc_t;

/* region must be at least sizeof(heap_block_t) bytes and live for the
   lifetime of heap's use. */
void heap_alloc_init(heap_alloc_t *heap, void *region, size_t region_size);

/* Returns a pointer to a zeroed-length (not zero-filled) block of at
   least `size` bytes, 16-byte aligned, or NULL if no free block is
   large enough. */
void *heap_alloc_alloc(heap_alloc_t *heap, size_t size);

/* Frees a pointer previously returned by heap_alloc_alloc(heap, ...).
   No-op on NULL. Coalesces with adjacent free blocks. */
void heap_alloc_free(heap_alloc_t *heap, void *ptr);

#endif /* LIBK_HEAP_ALLOC_H */
