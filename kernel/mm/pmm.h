#ifndef KERNEL_MM_PMM_H
#define KERNEL_MM_PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE 4096u

/* Parses the Multiboot2 memory map at mbi_addr (the pointer boot.asm
   passed through from EBX) and builds the free-frame bitmap. Must run
   before any pmm_alloc_frame()/pmm_free_frame() call. */
void pmm_init(uint32_t mbi_addr);

/* Returns the physical address of a free 4KiB frame (already marked
   used), or 0 if none remain -- 0 is never a valid allocated frame
   (physical address 0 is permanently reserved), so it doubles as the
   failure sentinel with no separate error out-param needed. */
uint64_t pmm_alloc_frame(void);

/* Milestone 21 (ADR 0021, copy-on-write fork): registers one MORE
   reference to an already-allocated frame (e.g. a second process's page
   table now also points at it, kernel/mm/vmm.c's vmm_fork_cow_page()) --
   does NOT allocate anything new. No-op on misuse (phys_addr not
   currently allocated) -- same internal-API contract-violation
   philosophy as pmm_free_frame()'s own no-op-on-misuse below. Every
   frame starts at refcount 1 the moment pmm_alloc_frame() hands it out
   -- this call is only ever needed to go beyond that single implicit
   owner. */
void pmm_frame_addref(uint64_t phys_addr);

/* Milestone 21 (ADR 0021): how many live references phys_addr
   currently has (1 for an ordinary exclusively-owned frame, 2+ only
   for a frame at least one pmm_frame_addref() call has touched). 0 if
   phys_addr isn't currently allocated at all. Lets a caller
   (vmm_handle_cow_fault()) tell "I'm the last reference -- just take
   ownership in place, no copy needed" apart from "still shared --
   must copy before writing", the standard real-COW optimization. */
uint32_t pmm_frame_refcount(uint64_t phys_addr);

/* Returns phys_addr to the free pool -- or, if it currently has MORE
   than one live reference (pmm_frame_addref()), just decrements the
   reference count and leaves the frame allocated (some other page
   table entry still needs it). No-op on misuse (unaligned, out of
   range, or already free) -- an internal-API contract violation, not
   external input that needs a parser-grade validation error. */
void pmm_free_frame(uint64_t phys_addr);

uint64_t pmm_frames_free(void);
uint64_t pmm_frames_total(void);

#endif /* KERNEL_MM_PMM_H */
