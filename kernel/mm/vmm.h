#ifndef KERNEL_MM_VMM_H
#define KERNEL_MM_VMM_H

#include <stdbool.h>
#include <stdint.h>

#define VMM_FLAG_WRITABLE (1ULL << 1)
/* No VMM_FLAG_USER yet: no ring 3 exists (Milestone 7), so every
   mapping this VMM creates is supervisor-only (U/S bit left 0) until
   userspace actually needs otherwise. */

/* Maps virt_addr -> phys_addr (both must be 4KiB-aligned), creating any
   missing intermediate page-table levels via the physical frame
   allocator. Returns false if virt_addr is already mapped (refuses to
   silently overwrite an existing mapping) or a table frame couldn't be
   allocated. Flushes the TLB for this page on success. */
bool vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

/* Unmaps virt_addr if mapped; no-op otherwise. Does NOT free the
   underlying physical frame -- that's the caller's job (pmm_free_frame),
   kept separate on purpose (small single-purpose functions). */
void vmm_unmap_page(uint64_t virt_addr);

#endif /* KERNEL_MM_VMM_H */
