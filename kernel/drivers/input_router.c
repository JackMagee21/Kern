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

void input_router_notify(uint32_t event, uint32_t x, uint32_t y)
{
    uint32_t target_pid = syscall_get_input_focus_pid();
    if (target_pid == 0) {
        return; /* nobody has ever subscribed -- silently drop, matching mouse.c's own lossy-queue contract */
    }

    task_t *target = scheduler_find_task(target_pid);
    if (target == NULL) {
        return; /* the subscriber has already exited -- nothing to deliver to */
    }

    ipc_message_t msg = { .fields = { event, x, y, 0 } };
    if (!ipc_send(target, &msg)) {
        return; /* subscriber's inbox is full -- same drop-on-full contract sys_ipc_send() already has */
    }

    if (event == INPUT_EVENT_CLICK) {
        click_count++;
    }

    /* Printed from the TRUSTED kernel side, not left to the receiving
       userspace process -- kernel/user/rt/ has no integer-to-string
       formatting (every existing demo only ever prints fixed literal
       strings, ADR 0024's own scope), so this is the only place able
       to report the EXACT (x, y) a smoke test can assert against,
       the same role console_write_hex() already plays for every other
       "prove precisely what happened" self-test marker in this
       codebase. Milestone 31: kept unconditional (drag/release events
       included, even though there can be many drag steps in one
       interaction) rather than only logging clicks -- a smoke test
       asserting on a specific drag step needs the same trusted,
       exact-value log line clicks already get; boot-time console
       volume from this was already accounted for by Milestone 30's own
       bounded console region fix. Spelled out as a word
       ("click"/"drag"/"release"), not the raw opcode hex, so a smoke
       test can grep for an exact, readable marker the same way every
       other one in this codebase already does, rather than needing to
       know the numeric encoding. */
    const char *event_name = (event == INPUT_EVENT_CLICK) ? "click"
                            : (event == INPUT_EVENT_DRAG) ? "drag"
                            : (event == INPUT_EVENT_RELEASE) ? "release"
                            : "unknown";
    /* Milestone 37 (ADR 0037): serial-only (console_log, not
       console_write) -- this fires on EVERY click/drag/release, so a
       real drag interaction alone can print dozens of these; keeping
       it on-screen would mean using the actual GUI generates a wall of
       scrolling text over the console's reserved region while the
       desktop is in normal use. Still reaches every existing smoke
       test unchanged -- they all grep serial output, never the
       screen. */
    console_log("[OK] input router: routed ");
    console_log(event_name);
    console_log(" to pid 0x");
    console_log_hex(target_pid);
    console_log(" at (0x");
    console_log_hex(x);
    console_log(", 0x");
    console_log_hex(y);
    console_log(")\n");
}

uint64_t input_router_get_click_count(void)
{
    return click_count;
}
