#ifndef KERNEL_MM_VMM_H
#define KERNEL_MM_VMM_H

#include <stdbool.h>
#include <stdint.h>

#define VMM_FLAG_WRITABLE (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2) /* Milestone 7: ring-3-accessible (U/S bit) */
#define VMM_FLAG_NX       (1ULL << 63) /* execute-disable (XD) leaf bit -- Intel SDM
                                           Vol. 3A Sec. 4.6, requires EFER.NXE=1
                                           (vmm_enable_nx()) or this bit is reserved
                                           and using it faults. Setting it on JUST the
                                           leaf entry is sufficient to block instruction
                                           fetch from that page regardless of
                                           intermediate levels (get_or_create_table
                                           never sets it on PDPT/PD/PT entries, so it
                                           can't accidentally block execution anywhere
                                           else) -- the SDM's "most restrictive wins"
                                           rule applies the same way it does to U/S and
                                           writable. */
/* boot.asm's pd_shared covers physical 0-8MiB (ADR 0001) with 2MiB
   identity pages. Since Milestone 19's general physical-memory
   direct-map (vmm_direct_map_init()/vmm_phys_to_virt()), this
   constraint applies ONLY to page-table BOOTSTRAP frames -- vmm.c's
   own get_or_create_table()/vmm_create_address_space()/
   vmm_direct_map_init() itself, which allocate the very page tables
   everything else (including the direct map) depends on, and so can
   never be bootstrapped via a direct map that doesn't exist yet at the
   point they're needed. DATA frames (an ELF segment's content,
   kernel/mm/elf_loader.c; a forked page's content,
   kernel/sched/task.c's task_fork()) no longer need this -- they use
   vmm_phys_to_virt() instead, and can be anywhere pmm_alloc_frame()
   hands back. pmm_alloc_frame hands out the lowest-numbered free frame
   first and every remaining caller of this constant runs early in
   boot, so in practice it holds; each caller panics instead of
   silently corrupting memory if it's ever violated, rather than
   assuming. */
#define VMM_IDENTITY_WINDOW_LIMIT 0x800000ULL

#define VMM_FLAG_OWNED    (1ULL << 9) /* bits 9-11 are AVL (available for OS use) at
                                          every page-table level per Intel SDM Vol. 3A
                                          Sec. 4.5 -- doesn't collide with any
                                          hardware-defined bit. Marks a LEAF mapping
                                          whose physical frame was pmm_alloc_frame()'d
                                          specifically for it (e.g. a process's private
                                          stack) as opposed to pointing at pre-existing/
                                          shared/static memory (e.g. the ring-3 demo
                                          program's code page, which lives in the
                                          kernel image itself and was never pmm-
                                          allocated). vmm_destroy_address_space() only
                                          pmm_free_frame()s a leaf's target if this bit
                                          is set -- omitting it on a shared mapping is
                                          what prevents a process's teardown from
                                          freeing memory another process (or the kernel
                                          image) still needs. */

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

/* Verifies the CPU actually supports the NX/XD feature (CPUID
   80000001h:EDX bit 20 -- the same extended leaf boot.asm's
   check_long_mode already confirmed is available before long mode was
   ever entered, so no need to re-check leaf 80000000h's own
   availability here) and sets IA32_EFER.NXE, panicking if the CPU
   doesn't support it rather than silently letting a later VMM_FLAG_NX
   mapping fault with a confusing reserved-bit #PF. Must run before any
   VMM_FLAG_NX mapping is ever walked by the CPU for a real access;
   called once from kernel_main, early, well before either mapping that
   currently uses VMM_FLAG_NX (the kernel heap, a process's stack) is
   created. */
void vmm_enable_nx(void);

/* Same as vmm_map_page(), but into an EXPLICIT address space
   (pml4_phys, as returned by vmm_create_address_space()) rather than
   whatever CR3 currently is. Needed to build a new process's page
   tables before that process is actually scheduled/active -- the
   caller's own (kernel) address space stays loaded throughout, which
   is what keeps the identity-mapping bootstrap trick (see
   VMM_IDENTITY_WINDOW_LIMIT below) working: every table frame this
   allocates is still reachable via the CALLER's identity map, not the
   target address space's (which has none -- see
   vmm_create_address_space()). vmm_map_page() is just this with
   vmm_current_pml4(). */
bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

/* Physical address of the top-level page table CR3 currently points
   at. */
uint64_t vmm_current_pml4(void);

/* Allocates a fresh top-level page table for a new process, with two
   entries COPIED from the CALLER's current address space: PML4[511]
   (kernel image + heap, see ADR 0004) and PML4[0] (boot.asm's low
   identity map). Copying the entry (a pointer to the same physical
   PDPT), not the tree it points to, means kernel code/data/heap stay
   identical and stay in sync (any later kernel-heap growth is
   automatically visible to every existing process too, since they all
   reference the same underlying PDPT frame) across every address space
   forever, not just at creation time. PML4[0] must be shared too, not
   just PML4[511]: kernel code that later runs under this process's CR3
   (a syscall or exception handler -- neither SYSCALL nor an interrupt
   switches CR3 on entry) still needs identity-mapped structures
   reachable (page-table walkers casting CR3 to a pointer, the VGA
   console's direct 0xB8000 access, etc). Safe to share: every
   identity-mapped entry is supervisor-only, so this grants kernel CODE
   broader reach without granting ring-3 code in the process anything
   new. See ADR 0009 for the real bug found before this was shared.
   Panics if a frame can't be allocated. */
uint64_t vmm_create_address_space(void);

/* Tears down a per-process address space created by
   vmm_create_address_space(): walks every PML4 entry EXCEPT [0] and
   [511] (the shared identity map and kernel half -- never touched,
   never freed, since they're owned by the kernel and every other
   address space too), recursively frees every PDPT/PD/PT frame the
   walk finds (always pmm-owned, since get_or_create_table always
   allocates them fresh for a new address space), and pmm_free_frame()s
   a leaf's target physical frame only if its VMM_FLAG_OWNED bit is
   set. Finally frees the PML4 frame itself.

   Caller's responsibility, not this function's: the address space
   being destroyed must NOT be the currently active one (CR3 must
   already point elsewhere) and nothing may still be executing on any
   stack this teardown is about to free. See ADR 0010 for why process
   exit can't do this teardown synchronously inside the exiting
   process's own syscall handler -- both of those conditions are still
   false at that point, the same category of hazard as ADR 0009's CR3-
   switch-timing bug. */
void vmm_destroy_address_space(uint64_t pml4_phys);

/* Returns true and sets *out_phys to the mapped physical frame if
   virt_addr is present; returns false (leaving *out_phys untouched)
   otherwise. General VA->PA lookup -- needed by a caller that wants to
   pmm_free_frame() what vmm_unmap_page() is about to discard, since
   vmm_unmap_page() deliberately doesn't do that itself (see its own
   doc comment below). */
bool vmm_translate(uint64_t virt_addr, uint64_t *out_phys);

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

/* Returns true only if virt_addr is present AND genuinely executable
   (XD/VMM_FLAG_NX clear at whatever level is the actual leaf) in the
   given address space -- i.e. reads back the real page-table state a
   VMM_FLAG_NX mapping actually produced, rather than trusting that the
   call site that created it got the flag right. Used by kernel_main's
   NX self-test: proving NX is enforced by deliberately executing
   NX-protected memory and observing the fault would need an exception-
   recovery mechanism this kernel doesn't have yet (a fault, unlike a
   trap, resumes AT the faulting instruction -- there's no safe generic
   way to skip past it without one), so this checks the actual
   resulting PTE bit instead -- a real, if more indirect, proof that
   the plumbing took effect. */
bool vmm_page_is_executable_in(uint64_t pml4_phys, uint64_t virt_addr);

/* Milestone 18 (ADR 0018, fork): called once per PRESENT leaf mapping
   found while walking pml4_phys's process-private region (PML4 entries
   1-510 -- the same range vmm_destroy_address_space() walks, excluding
   the shared identity map [0] and kernel half [511]), with that page's
   virtual address, physical frame, and its flags (WRITABLE/USER/NX/
   OWNED only -- PRESENT and the address bits are masked out; the
   caller decides what to do with them, e.g. re-applying them verbatim
   to a new mapping). va is reconstructed assuming a LOWER-HALF
   canonical address (PML4 index < 256, no sign-extension needed) --
   true for every process-private region this kernel ever creates
   (index 1, kernel/sched/task.c), not asserted/checked here since nothing
   in this codebase ever uses a higher-half process-private index. */
typedef void (*vmm_page_visitor_t)(uint64_t va, uint64_t phys, uint64_t flags, void *ctx);

/* Walks every present leaf mapping in pml4_phys's process-private
   region and calls visitor once per page, in ascending virtual-address
   order (same nested PML4/PDPT/PD/PT traversal as
   vmm_destroy_address_space(), read-only). Panics if it finds a huge
   (2MiB/1GiB) page in that region, same defensive check
   vmm_destroy_address_space() already makes -- nothing in this codebase
   creates one there, so hitting one would mean a logic bug elsewhere. */
void vmm_for_each_user_page(uint64_t pml4_phys, vmm_page_visitor_t visitor, void *ctx);

/* Milestone 19: maps the full 4GiB physical-address range pmm.h's
   bitmap tracks at a fixed high virtual base (2MiB pages, supervisor-
   only), so any physical frame -- not just one inside
   VMM_IDENTITY_WINDOW_LIMIT -- can be written to directly via
   vmm_phys_to_virt(). MUST be called before the first
   vmm_create_address_space() -- see vmm.c's doc comment on this
   function for why. Panics if a page-table bootstrap frame it needs
   falls outside VMM_IDENTITY_WINDOW_LIMIT (this function's OWN table
   frames still need that, even though its whole purpose is to remove
   the requirement for everything built on top of it afterward -- see
   vmm.c). */
void vmm_direct_map_init(void);

/* Translates a physical address into its direct-map virtual address
   (vmm_direct_map_init() must have already run). Does NOT validate
   phys_addr is actually a frame pmm_alloc_frame() ever handed out --
   same trust boundary as every other consumer of a raw physical
   address in this codebase (get_or_create_table, vmm_translate's
   callers, etc.): internal callers are trusted, not re-validated at
   every layer. */
uint64_t vmm_phys_to_virt(uint64_t phys_addr);

#endif /* KERNEL_MM_VMM_H */
