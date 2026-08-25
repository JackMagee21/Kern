/* Milestone 29 (ADR 0029): proves a REAL hardware input event (a
   genuine PS/2 left-click, decoded by the actual mouse driver and
   IRQ12, kernel/drivers/mouse.c) reaches a specific ring-3 process via
   IPC -- the first time this kernel has ever delivered a hardware
   event to userspace at all (every prior consumer of mouse.c's own
   queues -- cursor_poll(), the shell's `mouse` command -- is
   kernel-side code). Deliberately a small, standalone demo, NOT a
   change to kernel/user/display_server.c: wiring THIS mechanism up to
   "and now the display server raises the clicked window" is its own
   later milestone (see ADR 0029's Decision on why that's out of this
   milestone's scope) -- this process exists purely to prove the
   delivery mechanism itself, in isolation, with a process whose whole
   job is receiving exactly one event and then exiting (so
   kernel_main's own deterministic reap-count self-test still holds
   for a plain headless boot with no external input ever injected --
   see this file's own doc comment on why it does NOT block forever
   waiting for a click that a normal test run will never send). */

#include <stdint.h>

#include "rt/syscall.h"
#include "input_protocol.h"

static const char msg_subscribed[] = "[OK] input focus demo: subscribed to hardware input events\n";
static const char msg_subscribe_failed[] = "[FAIL] input focus demo: sys_input_subscribe failed\n";
static const char msg_resubscribe_ok[] = "[OK] input focus demo: a second sys_input_subscribe correctly failed (already subscribed)\n";
static const char msg_resubscribe_bug[] = "[FAIL] input focus demo: a second sys_input_subscribe incorrectly succeeded\n";
static const char msg_click_ok[] = "[OK] input focus demo: received a real routed click event\n";
static const char msg_click_bad[] = "[FAIL] input focus demo: received message had the wrong opcode\n";

int main(void)
{
    if (sys_input_subscribe() == (uint64_t)-1) {
        sys_write(msg_subscribe_failed, sizeof(msg_subscribe_failed) - 1);
        return 1;
    }
    sys_write(msg_subscribed, sizeof(msg_subscribed) - 1);

    /* Proves subscription is genuinely exclusive, fully self-contained
       (no second process needed for this half of the proof) -- the
       SAME process that just successfully subscribed cannot do so
       again, the same shape sys_fb_acquire()'s own re-acquire
       self-check already established (Milestone 27). */
    if (sys_input_subscribe() != (uint64_t)-1) {
        sys_write(msg_resubscribe_bug, sizeof(msg_resubscribe_bug) - 1);
        return 1;
    }
    sys_write(msg_resubscribe_ok, sizeof(msg_resubscribe_ok) - 1);

    /* Blocks until a REAL click is injected (kernel/drivers/cursor.c's
       own edge-detection, driven by an actual IRQ12 PS/2 report) --
       kernel_main's own self-test (syscall_get -- actually
       input_router_get_click_count()) only advances past its own wait
       once this process has been fully created and subscribed, and the
       kernel-side smoke test injects a real synthetic click via the
       QEMU monitor once the shell prompt confirms boot has settled
       (tests/qemu/test_input_focus_selftest.sh) -- a plain `make run`
       or headless test run that never sends one leaves this process
       blocked harmlessly for the rest of that boot, same as the shell
       itself waiting on keyboard input, not treated as a hang. */
    ipc_message_t msg;
    sys_ipc_recv(&msg);

    if (msg.fields[0] != INPUT_EVENT_CLICK) {
        sys_write(msg_click_bad, sizeof(msg_click_bad) - 1);
        return 1;
    }

    sys_write(msg_click_ok, sizeof(msg_click_ok) - 1);
    return 0;
}
