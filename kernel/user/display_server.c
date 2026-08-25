/* Milestone 27 (ADR 0027) / Milestone 28 (ADR 0028): the piece of
   Desktop.md's GUI arc that actually owns pixels on screen. Milestone
   27 proved the client-server model works at all with exactly one
   client; Milestone 28 extends this to WINDOWS_TOTAL (2) clients with
   real spatial overlap, proving z-order compositing works -- still no
   window chrome, no move/close, and (deliberately, see ADR 0028's
   Decision) no real input-driven focus/raising yet, that's its own
   later milestone.

   Claims sole ownership of the real framebuffer via sys_fb_acquire()
   (kernel-ENFORCED, Milestone 27), then serves each client in turn
   over the tiny request/grant/present protocol (display_protocol.h):
   grants a canvas no larger than its own fixed maximum -- regardless
   of what was actually requested, this IS "the server enforces the
   bound" Desktop.md describes -- at a cascaded position that overlaps
   the previous window, composites it (Milestone 26's shared-memory
   mechanism), and ACKs the client before moving on to the next one.

   Serving clients in a strict, unconditional sequential loop (recv
   REQUEST, send GRANT, recv PRESENT, composite, send ACK -- THEN move
   to the next client) is what makes z-order correctness follow
   directly from PRESENTATION order: since every window is fully
   OPAQUE, painting each one in turn naturally makes a later window's
   pixels win over an earlier one's in any overlapping region, with no
   separate compositing/redraw pass needed at all. This loop is only
   safe to write this simply because the CLIENTS themselves guarantee
   client B's REQUEST cannot reach this server until client A's own
   ACK has already been sent (kernel/user/display_client_a.c's own
   go-signal hand-off) -- if clients could request in arbitrary
   interleaved order, this server would need a real per-pid state
   machine instead of a flat loop; see ADR 0028 for why that complexity
   was deliberately pushed onto the (simpler to reason about) client
   side instead of built here. */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"

/* This server's own fixed policy: never grant a client more canvas
   than this, no matter what it asks for. Placed here, not in
   display_protocol.h, since it's this SPECIFIC server's own decision,
   not part of the shared wire format -- a different server built later
   could reasonably choose a different maximum. */
#define MAX_CANVAS_W 200u
#define MAX_CANVAS_H 150u

/* Milestone 28: a fixed cascade placement, one entry per client, in
   the exact order this server expects to serve them (client A first,
   client B second -- enforced by the go-signal hand-off, not assumed).
   Window 1 is offset (+50, +50) from window 0 so their granted
   200x150/200x150 canvases genuinely overlap (a 150x100 shared
   region) -- proving actual occlusion, not just two side-by-side
   rectangles that happen not to touch. No window manager/general
   placement logic exists yet (Desktop.md milestone 5's later slice);
   both positions are comfortably inside any real screen mode this
   kernel negotiates (Milestone 23's own 1024x768 request). */
#define WINDOWS_TOTAL 2u
static const uint32_t window_x[WINDOWS_TOTAL] = { 100u, 150u };
static const uint32_t window_y[WINDOWS_TOTAL] = { 100u, 150u };

static const char msg_acquired[] = "[OK] display server: framebuffer acquired\n";
static const char msg_acquire_failed[] = "[FAIL] display server: sys_fb_acquire failed\n";
static const char msg_reacquire_ok[] = "[OK] display server: a second sys_fb_acquire correctly failed (framebuffer already owned)\n";
static const char msg_reacquire_bug[] = "[FAIL] display server: a second sys_fb_acquire incorrectly succeeded\n";
static const char msg_present_ok[] = "[OK] display server: presented window 0x";
static const char msg_present_failed[] = "[FAIL] display server: mapping or presenting a client's buffer failed\n";
static const char msg_all_ok[] = "[OK] display server: all windows presented in z-order\n";

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
       display_client_a.c/display_client_b.c for the cross-process half
       of this proof. */
    if (sys_fb_acquire() != (uint64_t)-1) {
        sys_write(msg_reacquire_bug, sizeof(msg_reacquire_bug) - 1);
        return 1;
    }
    sys_write(msg_reacquire_ok, sizeof(msg_reacquire_ok) - 1);

    for (uint32_t i = 0; i < WINDOWS_TOTAL; i++) {
        ipc_message_t req;
        sys_ipc_recv(&req); /* blocks until this window's DISPLAY_OP_REQUEST arrives */

        uint64_t requested_w = req.fields[1];
        uint64_t requested_h = req.fields[2];
        uint64_t granted_w = requested_w < MAX_CANVAS_W ? requested_w : MAX_CANVAS_W;
        uint64_t granted_h = requested_h < MAX_CANVAS_H ? requested_h : MAX_CANVAS_H;
        uint64_t x = window_x[i];
        uint64_t y = window_y[i];

        ipc_message_t grant = {
            .fields = { DISPLAY_OP_GRANT, x, y, (granted_w << 32) | granted_h }
        };
        sys_ipc_send(req.sender_pid, &grant);

        ipc_message_t pres;
        sys_ipc_recv(&pres); /* blocks until THIS SAME client's DISPLAY_OP_PRESENT arrives -- guaranteed to be this one, not the other client's, by the clients' own go-signal ordering */

        uint64_t shm_id = pres.fields[1];
        uint64_t va = sys_shm_map(shm_id);
        if (va == 0) {
            sys_write(msg_present_failed, sizeof(msg_present_failed) - 1);
            return 1;
        }

        if (sys_fb_present(x, y, granted_w, granted_h, (const void *)(uintptr_t)va) == (uint64_t)-1) {
            sys_write(msg_present_failed, sizeof(msg_present_failed) - 1);
            return 1;
        }

        /* Only sent once sys_fb_present() above has actually returned
           -- this is what lets client A's own go-signal to client B
           (display_client_a.c) truthfully mean "my canvas is already
           on screen", turning the resulting z-order into a fact this
           server's OWN sequencing guarantees, not a race either client
           has to assume. */
        ipc_message_t ack = { .fields = { DISPLAY_OP_ACK, 0, 0, 0 } };
        sys_ipc_send(req.sender_pid, &ack);

        sys_write(msg_present_ok, sizeof(msg_present_ok) - 1);
        char digit = (char)('0' + i);
        sys_write(&digit, 1);
        sys_write("\n", 1);
    }

    sys_write(msg_all_ok, sizeof(msg_all_ok) - 1);
    return 0;
}
