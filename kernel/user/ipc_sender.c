/* Milestone 26 (ADR 0026): the sending half of the IPC + shared-memory
   demo. Learns the receiver's pid from a bootstrap message
   kernel_main itself injects directly (a kernel-side ipc_send() call,
   not a syscall -- kernel_main is trusted/privileged code acting as
   this demo's own "init") before this process ever runs -- there's no
   argv/envp yet (ADR 0024's own Known limitations) and the receiver is
   a genuine sibling process, not this one's parent or child, so pid
   discovery needs SOME bootstrap mechanism; reusing kernel_main's own
   already-privileged position as the simplest one, rather than
   inventing a general name-service this milestone doesn't need.

   Creates a shared-memory object, writes a known pattern into it, then
   hands the object's id to the receiver via a real sys_ipc_send() --
   ipc_receiver.c maps the SAME object and checks the pattern read back
   correctly. */

#include <stdint.h>

#include "rt/syscall.h"

#define SHARED_PATTERN 0xcafebabedeadbeefULL

static const char msg_ok[] = "[OK] ipc demo sender: wrote shared pattern and handed off the shm id\n";
static const char msg_bad[] = "[FAIL] ipc demo sender: shm_create/shm_map/ipc_send failed\n";

int main(void)
{
    ipc_message_t boot_msg;
    sys_ipc_recv(&boot_msg); /* kernel_main's own bootstrap message: fields[0] = the receiver's pid */
    uint64_t receiver_pid = boot_msg.fields[0];

    uint64_t shm_id = sys_shm_create(4096);
    uint64_t va = shm_id != 0 ? sys_shm_map(shm_id) : 0;
    if (va == 0) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    uint64_t *shared = (uint64_t *)(uintptr_t)va;
    *shared = SHARED_PATTERN;

    ipc_message_t msg = { .fields = { shm_id, 0, 0, 0 } };
    if (sys_ipc_send(receiver_pid, &msg) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    sys_write(msg_ok, sizeof(msg_ok) - 1);
    return 0;
}
