#ifndef KERNEL_IPC_SHM_H
#define KERNEL_IPC_SHM_H

#include <stdint.h>

#include "../sched/task.h"

/* Milestone 26 (ADR 0026): named shared-memory objects -- lets two
   otherwise-fully-isolated processes (Milestone 9's whole point) share
   a block of physical memory on purpose, for bulk data (pixels) that
   would be far too slow to copy through kernel/ipc/msgqueue.c's small
   fixed-field messages one message at a time. Deliberately narrower
   than general VMA tracking (a real per-process memory map, `future.md`'s
   long-deferred item): this is a small, fixed-capacity table of named
   objects with a size and a frame list, not a general "arbitrary region
   with arbitrary permissions" abstraction -- built because THIS
   concrete need (a client and a display server sharing a pixel buffer)
   exists now, not because VMAs themselves were needed for their own
   sake (CLAUDE.md: don't build for hypothetical future requirements).

   Cleanup is intentionally NOT a separate explicit "destroy" API --
   frames are refcounted (kernel/mm/pmm.h's EXISTING pmm_frame_addref()/
   pmm_free_frame(), the same mechanism Milestone 21's copy-on-write
   fork already established) and every mapper's own mapping is
   VMM_FLAG_OWNED in ITS OWN address space, so a mapper's ordinary
   process exit (vmm_destroy_address_space(), unchanged since ADR 0010)
   already drops exactly one reference per frame it mapped -- the
   physical memory is only actually freed once EVERY mapper has exited,
   the identical "last reference frees it" pattern COW fork already
   proved correct (ADR 0021), reused here instead of inventing a new
   cleanup mechanism. */

/* Fixed VA region every process reserves for shared-memory mappings --
   between the ELF image's own code (0x8000400000, kernel/user/user.ld)
   and the user stack (0x8000600000, kernel/sched/task.c's
   USER_STACK_VIRT_BASE), which leaves roughly 2MiB of headroom no
   other convention currently claims. Capped well under that gap (see
   SHM_MAX_TOTAL_BYTES) so a shm-heavy process can never grow into the
   stack's own address range. */
#define SHM_VIRT_BASE 0x0000008000500000ULL
#define SHM_MAX_TOTAL_BYTES (1u * 1024u * 1024u) /* 1MiB budget per process, generous for this milestone's actual needs */

/* Allocates num_pages = ceil(size / 4KiB) fresh physical frames (each
   getting pmm_alloc_frame()'s normal refcount of 1 -- the CREATOR's own
   implicit reference, which becomes real the moment the creator itself
   calls shm_map() on the returned id, same as every other mapper) and
   registers them under a new object id. Returns 0 (never a valid id --
   real ids start at 1, matching this codebase's other "0 is never
   valid" sentinel conventions, e.g. task ids) if size is 0, exceeds
   SHM_MAX_PAGES_PER_OBJECT, or the object table is full. Does NOT map
   the new object into any address space -- the creator must still call
   shm_map() itself, same as any other process would, so every mapper
   (creator included) ends up with exactly one real OWNED reference,
   not a phantom table-only one that would never get dropped. */
uint32_t shm_create(uint64_t size);

/* Maps shm_id into task's OWN address space (task->pml4), at the next
   free slot in task's own per-task VA bump region (task->shm_next_va,
   task.h) -- VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_OWNED |
   VMM_FLAG_NX (data, never code, same W^X reasoning as every other
   writable user mapping in this codebase). pmm_frame_addref()s each
   frame once, so task's own eventual vmm_destroy_address_space() (on
   exit) correctly drops exactly this one reference -- see this file's
   own top-of-file doc comment for why that's the object's entire
   cleanup mechanism. Returns 0 (never a valid VA in this kernel's
   canonical-address convention -- every real mapping lives at
   SHM_VIRT_BASE or above) if shm_id doesn't exist or task's own VA
   budget (SHM_MAX_TOTAL_BYTES) is exhausted. Sets *out_size to the
   object's real size in bytes if out_size != NULL. */
uint64_t shm_map(uint32_t shm_id, task_t *task, uint64_t *out_size);

#endif /* KERNEL_IPC_SHM_H */
