#ifndef KERNEL_MM_ELF_LOADER_H
#define KERNEL_MM_ELF_LOADER_H

#include <stdbool.h>
#include <stdint.h>

/* Loads a validated (libk/elf.h) ELF64 static executable image --
   already fully present in kernel memory, e.g. the embedded
   build/kernel/user/hello.elf blob (kernel/sched/user_elf_blob.asm) --
   into a process's OWN address space: every PT_LOAD program header
   becomes one or more freshly allocated, VMM_FLAG_OWNED,
   VMM_FLAG_USER page mappings, so vmm_destroy_address_space() reclaims
   them on process exit (ADR 0010) the same as the private user stack
   task_create_user() maps separately. Permissions are derived
   PER-SEGMENT from p_flags (PF_W -> VMM_FLAG_WRITABLE, !PF_X ->
   VMM_FLAG_NX) -- real per-segment W^X, replacing the single
   hand-mapped, always-executable-never-writable demo code page every
   process shared read-only through Milestone 7-16 (the retired
   kernel/sched/user_demo.asm). Bytes beyond p_filesz up
   to p_memsz within a segment (.bss) are zero-filled, not copied.

   Simplifying assumption (see libk/elf.h's elf64_validate_load_segment
   doc comment): every PT_LOAD segment's p_vaddr must be 4KiB-aligned --
   true for kernel/user/user.ld's explicit per-section ALIGN(4K), not
   true in general for an arbitrary ELF file (which may pack multiple
   segments' boundaries within a shared page for file-size efficiency).
   A general-purpose loader would need to handle that; this one doesn't
   need to yet, since it only ever loads images built by this repo's own
   user.ld.

   Each segment's destination frames are allocated fresh per process (no
   shared/copy-on-write text pages, unlike the old demo's design) --
   simpler and correct, at the cost of duplicating physical memory for
   identical program text across processes running the same binary;
   revisit alongside demand paging/COW (already a flagged future-work
   item, ADR 0004/0009). Every destination frame must fall within
   VMM_IDENTITY_WINDOW_LIMIT (vmm.h) to be directly writable via its own
   physical address as a pointer while filling it in -- panics rather
   than silently corrupting memory if that's ever violated, same
   discipline as vmm.c's own page-table bootstrap frames.

   On success, returns true and sets *out_entry to e_entry (the address
   task_create_user() sets the new process's trap-frame RIP to).
   Returns false if the image fails ELF64 validation (a real, expected
   input-validation outcome per CLAUDE.md's parser-security rule, not a
   kernel bug) -- the caller must not retry loading into the same
   address space; it's still only partially populated at that point and
   should just be torn down (vmm_destroy_address_space()). Panics (not a
   false return) on an allocation/mapping failure, matching every other
   task_create_user() failure mode -- those indicate a kernel resource
   exhaustion or invariant violation, not bad input. */
bool elf_load(uint64_t pml4_phys, const uint8_t *image, uint64_t image_size, uint64_t *out_entry);

#endif /* KERNEL_MM_ELF_LOADER_H */
