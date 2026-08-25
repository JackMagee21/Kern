/* Milestone 26 (ADR 0026): the receiving half of the IPC + shared-
   memory demo. Its OWN first action is sys_ipc_recv() -- no
   prerequisite work at all, unlike ipc_sender.c (which must itself
   receive a bootstrap message from kernel_main before it can even
   begin) -- so this call is reliably the one that actually blocks,
   proving sys_ipc_recv() (and the scheduler_block_current()/wake()
   primitive underneath it, Milestone 25) genuinely blocks and resumes,
   not just that it dispatches without crashing.

   Once it receives the sender's message (fields[0] = a shared-memory
   object id the sender already wrote a known pattern into), it maps
   that SAME object into its OWN, completely independent address space
   and reads the pattern back -- the actual proof that shared memory
   really is shared across two genuinely isolated processes (Milestone
   9), not aliased by coincidence. */

#include <stdint.h>

#include "rt/syscall.h"

#define SHARED_PATTERN 0xcafebabedeadbeefULL

static const char msg_ok[] = "[OK] ipc/shm self-test passed: shared memory pattern verified via IPC handoff\n";
static const char msg_bad[] = "[FAIL] ipc/shm self-test: shared memory pattern mismatch after IPC handoff\n";

int main(void)
{
    ipc_message_t msg;
    sys_ipc_recv(&msg); /* blocks until the sender's message arrives */

    uint64_t shm_id = msg.fields[0];
    uint64_t va = sys_shm_map(shm_id);
    if (va == 0) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    const uint64_t *shared = (const uint64_t *)(uintptr_t)va;
    if (*shared == SHARED_PATTERN) {
        sys_write(msg_ok, sizeof(msg_ok) - 1);
    } else {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
    }

    return 0;
}
