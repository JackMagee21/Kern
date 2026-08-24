#include <stdint.h>

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../panic.h"
#include "../../libk/heap_alloc.h"

/*
 * PML4[511]:PDPT[511] -- the very last 1GiB of the address space, and
 * deliberately not the same PDPT slot (510) boot.asm's kernel-image
 * mapping already occupies (0xFFFFFFFF80000000, see ADR 0001), so this
 * heap gets its own dedicated region built fresh via vmm_map_page()
 * rather than extending/reinterpreting the boot-time 2MiB mapping.
 */
#define KERNEL_HEAP_VIRT_BASE 0xFFFFFFFFC0000000ULL

/* 1MiB to start (ADR 0004): enough to prove the allocator works and
   back early kernel data structures; grows on demand is future work,
   not needed by anything yet. */
#define KERNEL_HEAP_INITIAL_PAGES 256u

static heap_alloc_t kernel_heap;

void heap_init(void)
{
    for (uint64_t i = 0; i < KERNEL_HEAP_INITIAL_PAGES; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            panic("heap_init: pmm exhausted while mapping the initial kernel heap");
        }

        uint64_t virt = KERNEL_HEAP_VIRT_BASE + i * PMM_FRAME_SIZE;
        /* W^X: heap memory is data, never code -- VMM_FLAG_NX means an
           overflow/corruption bug here can't be turned into arbitrary
           code execution by jumping into attacker-controlled heap
           content. Requires vmm_enable_nx() to have already run
           (kernel_main calls it before heap_init()). */
        if (!vmm_map_page(virt, phys, VMM_FLAG_WRITABLE | VMM_FLAG_NX)) {
            panic("heap_init: vmm_map_page failed for the initial kernel heap");
        }
    }

    heap_alloc_init(&kernel_heap, (void *)KERNEL_HEAP_VIRT_BASE,
                     (size_t)KERNEL_HEAP_INITIAL_PAGES * PMM_FRAME_SIZE);
}

void *kmalloc(size_t size)
{
    return heap_alloc_alloc(&kernel_heap, size);
}

void kfree(void *ptr)
{
    heap_alloc_free(&kernel_heap, ptr);
}
