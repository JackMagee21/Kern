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

/* Returns phys_addr to the free pool. No-op on misuse (unaligned, out
   of range, or already free) -- an internal-API contract violation,
   not external input that needs a parser-grade validation error. */
void pmm_free_frame(uint64_t phys_addr);

uint64_t pmm_frames_free(void);
uint64_t pmm_frames_total(void);

#endif /* KERNEL_MM_PMM_H */
