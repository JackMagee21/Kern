#include <stddef.h>
#include <stdint.h>

#include "shm.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../panic.h"

#define SHM_MAX_OBJECTS 16
#define SHM_MAX_PAGES_PER_OBJECT (SHM_MAX_TOTAL_BYTES / PMM_FRAME_SIZE)

typedef struct {
    int in_use;
    int has_mapper; /* has ANY shm_map() call succeeded for this object yet? see shm_map()'s own comment */
    uint32_t num_pages;
    uint64_t size;
    uint64_t frames[SHM_MAX_PAGES_PER_OBJECT];
} shm_object_t;

static shm_object_t shm_objects[SHM_MAX_OBJECTS];

uint32_t shm_create(uint64_t size)
{
    if (size == 0) {
        return 0;
    }
    uint32_t num_pages = (uint32_t)((size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    if (num_pages > SHM_MAX_PAGES_PER_OBJECT) {
        return 0;
    }

    for (uint32_t slot = 0; slot < SHM_MAX_OBJECTS; slot++) {
        if (shm_objects[slot].in_use) {
            continue;
        }

        for (uint32_t i = 0; i < num_pages; i++) {
            uint64_t frame = pmm_alloc_frame();
            if (frame == 0) {
                /* pmm exhausted mid-allocation -- a real (if rare)
                   resource-exhaustion outcome, not a kernel bug: roll
                   back what was already allocated rather than leak it. */
                for (uint32_t j = 0; j < i; j++) {
                    pmm_free_frame(shm_objects[slot].frames[j]);
                }
                return 0;
            }
            shm_objects[slot].frames[i] = frame;
        }

        shm_objects[slot].num_pages = num_pages;
        shm_objects[slot].size = size;
        shm_objects[slot].has_mapper = 0;
        shm_objects[slot].in_use = 1;
        return slot + 1; /* 0 reserved as "invalid id" */
    }

    return 0; /* object table full */
}

uint64_t shm_map(uint32_t shm_id, task_t *task, uint64_t *out_size)
{
    if (shm_id == 0 || shm_id > SHM_MAX_OBJECTS) {
        return 0;
    }
    shm_object_t *obj = &shm_objects[shm_id - 1];
    if (!obj->in_use) {
        return 0;
    }

    uint64_t mapping_bytes = (uint64_t)obj->num_pages * PMM_FRAME_SIZE;
    if (task->shm_next_va + mapping_bytes > SHM_VIRT_BASE + SHM_MAX_TOTAL_BYTES) {
        return 0; /* this task's own VA budget exhausted */
    }

    /* pmm_alloc_frame() (shm_create()) already gave each frame its own
       implicit refcount of 1 -- the SAME "one implicit owner" every
       other pmm_alloc_frame() caller in this codebase already gets
       (kernel/mm/pmm.c's own doc comment). The FIRST shm_map() call
       for this object is what turns that implicit reference into a
       real one (this mapper's own OWNED PTE) -- it must NOT also
       addref, or the frame would start life with refcount 2 while only
       ONE mapping actually exists, and that first mapper's eventual
       exit would only ever bring it back to 1, never 0: a permanent
       leak, never freed even after every real mapper has gone. Every
       mapper AFTER the first (a second, third, ... process also
       calling shm_map() on the same id) DOES need a real addref, since
       each is a genuinely new OWNED reference beyond the one
       pmm_alloc_frame() already accounted for. Exactly the same
       "the pre-existing reference is untouched, only the NEW one gets
       counted" split vmm_fork_cow_page() already established for COW
       (ADR 0021) -- found and fixed here by reasoning through the
       actual observed process-lifecycle frame-leak self-test failure
       on the first real boot attempt, not guessed in advance. */
    uint64_t base_va = task->shm_next_va;
    for (uint32_t i = 0; i < obj->num_pages; i++) {
        if (!vmm_map_page_in(task->pml4, base_va + (uint64_t)i * PMM_FRAME_SIZE, obj->frames[i],
                              VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_OWNED | VMM_FLAG_NX)) {
            /* A fresh VA range this task has never mapped before failing
               to map would mean a logic bug elsewhere (e.g. shm_next_va
               bookkeeping corrupted), not recoverable bad input -- same
               panic-on-unexpected-mapping-failure stance every other
               task_create*()-style caller in this codebase already
               takes. */
            panic("shm_map: vmm_map_page_in failed on a fresh VA range");
        }
        if (obj->has_mapper) {
            pmm_frame_addref(obj->frames[i]);
        }
    }
    obj->has_mapper = 1;
    task->shm_next_va = base_va + mapping_bytes;

    if (out_size != NULL) {
        *out_size = obj->size;
    }
    return base_va;
}
