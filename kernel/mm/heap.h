#ifndef KERNEL_MM_HEAP_H
#define KERNEL_MM_HEAP_H

#include <stddef.h>

/* Maps and initializes the initial kernel heap region. Must run after
   pmm_init(); panics if a frame or mapping can't be obtained (there's
   no fallback -- a kernel that can't establish its own heap this early
   in boot has nothing safe left to do). */
void heap_init(void);

void *kmalloc(size_t size);
void kfree(void *ptr);

#endif /* KERNEL_MM_HEAP_H */
