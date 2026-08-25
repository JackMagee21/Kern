#include <stddef.h>
#include <stdint.h>

#include "input_router.h"
#include "console.h"
#include "../arch/x86_64/syscall.h"
#include "../sched/scheduler.h"
#include "../sched/task.h"
#include "../ipc/msgqueue.h"
#include "../user/input_protocol.h"

static uint64_t click_count;

void input_router_notify_click(uint32_t x, uint32_t y)
{
    uint32_t target_pid = syscall_get_input_focus_pid();
    if (target_pid == 0) {
        return; /* nobody has ever subscribed -- silently drop, matching mouse.c's own lossy-queue contract */
    }

    task_t *target = scheduler_find_task(target_pid);
    if (target == NULL) {
        return; /* the subscriber has already exited -- nothing to deliver to */
    }

    ipc_message_t msg = { .fields = { INPUT_EVENT_CLICK, x, y, 0 } };
    if (!ipc_send(target, &msg)) {
        return; /* subscriber's inbox is full -- same drop-on-full contract sys_ipc_send() already has */
    }

    click_count++;

    /* Printed from the TRUSTED kernel side, not left to the receiving
       userspace process -- kernel/user/rt/ has no integer-to-string
       formatting (every existing demo only ever prints fixed literal
       strings, ADR 0024's own scope), so this is the only place able
       to report the EXACT (x, y) a smoke test can assert against,
       the same role console_write_hex() already plays for every other
       "prove precisely what happened" self-test marker in this
       codebase. */
    console_write("[OK] input router: routed a click to pid 0x");
    console_write_hex(target_pid);
    console_write(" at (0x");
    console_write_hex(x);
    console_write(", 0x");
    console_write_hex(y);
    console_write(")\n");
}

uint64_t input_router_get_click_count(void)
{
    return click_count;
}
