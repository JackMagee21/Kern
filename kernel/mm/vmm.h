#ifndef KERNEL_MM_VMM_H
#define KERNEL_MM_VMM_H

#include <stdbool.h>
#include <stdint.h>

#define VMM_FLAG_WRITABLE (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2) /* Milestone 7: ring-3-accessible (U/S bit) */

/* Maps virt_addr -> phys_addr (both must be 4KiB-aligned), creating any
   missing intermediate page-table levels via the physical frame
   allocator. Returns false if virt_addr is already mapped (refuses to
   silently overwrite an existing mapping) or a table frame couldn't be
   allocated. Flushes the TLB for this page on success.

   VMM_FLAG_USER propagates to every intermediate table entry along the
   walk (the U/S bit is effectively ANDed across all levels -- a
   supervisor-only intermediate entry blocks ring-3 access to
   everything under it regardless of the leaf's own bit), upgrading a
   pre-existing supervisor-only entry to user-accessible if needed, not
   just newly-created ones. This is necessary, not just convenient:
   PML4[511] alone spans the entire top-2GiB kernel region (kernel
   image, heap, and every "dedicated" user region all fall under one
   PML4 entry, since mcmodel=kernel's whole -2GB range is one PML4
   slot), so a user mapping's walk WILL hit intermediate tables an
   earlier non-user mapping already created. Upgrading them is safe:
   every other region's own LEAF entries stay supervisor-only and
   independently block ring-3 access regardless of what an intermediate
   table permits, so this can't expose anything beyond the new mapping
   actually being created. */
bool vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

/* Unmaps virt_addr if mapped; no-op otherwise. Does NOT free the
   underlying physical frame -- that's the caller's job (pmm_free_frame),
   kept separate on purpose (small single-purpose functions). */
void vmm_unmap_page(uint64_t virt_addr);

/* Returns true only if every byte in [addr, addr+length) falls within
   pages that are present AND user-accessible (U/S=1 at every level of
   the walk). Syscall handlers MUST call this before dereferencing any
   user-supplied pointer/length (CLAUDE.md: "never dereference
   user-supplied pointers/lengths without validating they're
   user-accessible") -- a plain "is this a valid kernel address" check
   is not enough, since a malicious or buggy user program could pass a
   kernel address and have the syscall read/write it with the kernel's
   own elevated privilege. */
bool vmm_is_user_range(uint64_t addr, uint64_t length);

#endif /* KERNEL_MM_VMM_H */
