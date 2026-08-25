/* Milestone 27 (ADR 0027): the first piece of Desktop.md's GUI arc that
   actually owns pixels on screen. Deliberately minimal -- one client,
   no window list, no damage tracking, no chrome -- the goal is only to
   prove the client-server display model works at all (Desktop.md:
   "the actual hard-unknown milestone") before any multi-window logic
   goes on top of it.

   Claims sole ownership of the real framebuffer via sys_fb_acquire()
   (kernel-ENFORCED, Milestone 27's own syscall.c), then speaks the
   tiny request/grant/present protocol (display_protocol.h) with
   exactly one client (kernel/user/display_client.c): the client asks
   for a canvas, this server grants one no larger than its own fixed
   maximum -- regardless of what was actually requested, this IS "the
   server enforces the bound" Desktop.md describes -- then composites
   whatever the client hands back (via the EXISTING Milestone 26
   shared-memory mechanism) onto the real screen. */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"

/* This server's own fixed policy: never grant a client more canvas
   than this, no matter what it asks for. Placed here, not in
   display_protocol.h, since it's this SPECIFIC server's own decision,
   not part of the shared wire format -- a different server built later
   could reasonably choose a different maximum. Position is likewise
   fixed (no window manager/placement logic exists yet, Desktop.md
   milestone 5) -- comfortably inside any real screen mode this kernel
   negotiates (Milestone 23's own 1024x768 request). */
#define MAX_CANVAS_W 200u
#define MAX_CANVAS_H 150u
#define CANVAS_X 100u
#define CANVAS_Y 100u

static const char msg_acquired[] = "[OK] display server: framebuffer acquired\n";
static const char msg_acquire_failed[] = "[FAIL] display server: sys_fb_acquire failed\n";
static const char msg_reacquire_ok[] = "[OK] display server: a second sys_fb_acquire correctly failed (framebuffer already owned)\n";
static const char msg_reacquire_bug[] = "[FAIL] display server: a second sys_fb_acquire incorrectly succeeded\n";
static const char msg_present_ok[] = "[OK] display server: presented the client's granted canvas\n";
static const char msg_present_failed[] = "[FAIL] display server: mapping or presenting the client's buffer failed\n";

int main(void)
{
    if (sys_fb_acquire() == (uint64_t)-1) {
        sys_write(msg_acquire_failed, sizeof(msg_acquire_failed) - 1);
        return 1;
    }
    sys_write(msg_acquired, sizeof(msg_acquired) - 1);

    /* Proves ownership is genuinely exclusive, fully self-contained
       (no second process needed for this half of the proof): the SAME
       process that just successfully acquired cannot do so again. See
       display_client.c for the cross-process half of this proof --
       the client's own later sys_fb_acquire() attempt, which the
       request/grant handshake below causally orders to happen AFTER
       this server's own acquire, no scheduling-order assumption
       needed (the same "blocking IPC enforces ordering by
       construction" reasoning Milestone 26's ADR already established). */
    if (sys_fb_acquire() != (uint64_t)-1) {
        sys_write(msg_reacquire_bug, sizeof(msg_reacquire_bug) - 1);
        return 1;
    }
    sys_write(msg_reacquire_ok, sizeof(msg_reacquire_ok) - 1);

    ipc_message_t req;
    sys_ipc_recv(&req); /* blocks until the client's DISPLAY_OP_REQUEST arrives */

    uint64_t requested_w = req.fields[1];
    uint64_t requested_h = req.fields[2];
    uint64_t granted_w = requested_w < MAX_CANVAS_W ? requested_w : MAX_CANVAS_W;
    uint64_t granted_h = requested_h < MAX_CANVAS_H ? requested_h : MAX_CANVAS_H;

    ipc_message_t grant = {
        .fields = { DISPLAY_OP_GRANT, CANVAS_X, CANVAS_Y, (granted_w << 32) | granted_h }
    };
    sys_ipc_send(req.sender_pid, &grant);

    ipc_message_t pres;
    sys_ipc_recv(&pres); /* blocks until the client's DISPLAY_OP_PRESENT arrives */

    uint64_t shm_id = pres.fields[1];
    uint64_t va = sys_shm_map(shm_id);
    if (va == 0) {
        sys_write(msg_present_failed, sizeof(msg_present_failed) - 1);
        return 1;
    }

    if (sys_fb_present(CANVAS_X, CANVAS_Y, granted_w, granted_h, (const void *)(uintptr_t)va) == (uint64_t)-1) {
        sys_write(msg_present_failed, sizeof(msg_present_failed) - 1);
        return 1;
    }

    sys_write(msg_present_ok, sizeof(msg_present_ok) - 1);
    return 0;
}
