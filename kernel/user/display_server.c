/* Milestone 27 (ADR 0027) / Milestone 28 (ADR 0028) / Milestone 30
   (ADR 0030): the piece of Desktop.md's GUI arc that actually owns
   pixels on screen. Milestone 27 proved the client-server model works
   at all with exactly one client; Milestone 28 extended this to
   WINDOWS_TOTAL (2) clients with real spatial overlap, proving z-order
   compositing works; Milestone 30 makes this server genuinely
   PERSISTENT -- after initial setup it never exits during a normal
   boot, subscribes to real hardware input (Milestone 29's mechanism),
   and actually RAISES a window in response to a real click, the first
   time anything in this GUI arc visibly reacts to input.

   Claims sole ownership of the real framebuffer via sys_fb_acquire()
   (kernel-ENFORCED, Milestone 27), then serves each client in turn
   over the tiny request/grant/present protocol (display_protocol.h):
   grants a canvas no larger than its own fixed maximum -- regardless
   of what was actually requested, this IS "the server enforces the
   bound" Desktop.md describes -- at a cascaded position that overlaps
   the previous window, composites it (Milestone 26's shared-memory
   mechanism), and ACKs the client before moving on to the next one.

   Serving the INITIAL setup in a strict, unconditional sequential loop
   (recv REQUEST, send GRANT, recv PRESENT, composite, send ACK -- THEN
   move to the next client) is what makes the STARTING z-order follow
   directly from PRESENTATION order: since every window is fully
   OPAQUE, painting each one in turn naturally makes a later window's
   pixels win over an earlier one's in any overlapping region. This
   loop is only safe to write this simply because the CLIENTS
   themselves guarantee client B's REQUEST cannot reach this server
   until client A's own ACK has already been sent
   (kernel/user/display_client_a.c's own go-signal hand-off) -- see ADR
   0028 for why that ordering complexity was deliberately pushed onto
   the client side.

   Unlike Milestones 27/28, this server does NOT exit once setup is
   done -- it enters a persistent loop, unconditionally waiting for the
   next message. A DISPLAY_OP_REQUEST/PRESENT from a THIRD client would
   currently be misrouted (this milestone still only expects exactly 2
   -- see Known limitations); the only OTHER message this loop actually
   understands is INPUT_EVENT_CLICK (kernel/user/input_protocol.h),
   Milestone 29's own delivery mechanism, which it now finally ACTS on:
   a real hit-test against the CURRENT z-order (topmost window first),
   and if the hit window isn't already on top, a raise (reorder the
   z-order, then recomposite every window bottom-to-top again --
   correctness follows the identical "presentation order = z-order"
   reasoning the initial setup already relies on). */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"
#include "input_protocol.h"

/* This server's own fixed policy: never grant a client more canvas
   than this, no matter what it asks for. Placed here, not in
   display_protocol.h, since it's this SPECIFIC server's own decision,
   not part of the shared wire format -- a different server built later
   could reasonably choose a different maximum. */
#define MAX_CANVAS_W 200u
#define MAX_CANVAS_H 150u

/* Milestone 30 (ADR 0030): windows now live at y >= 480 -- deliberately
   AT OR BEYOND kernel/drivers/fbconsole.c's own FBCONSOLE_MAX_HEIGHT_PX
   (480, that exact value, duplicated here since userspace code can't
   include a kernel header -- the same "plain documented constant, both
   sides hardcode consistently" pattern this codebase already uses for
   syscall numbers, ADR 0024) -- so console text scrolling (bounded to
   [0, 480) as of this same milestone) can NEVER shift them again. This
   is a REAL fix, not a smaller window for the same bug: Milestone 28
   found and explicitly flagged (ADR 0028's Known limitations) that
   windows placed within the console's own scrolling region drift from
   their nominal coordinates once enough boot text accumulates -- which
   would have made THIS milestone's own hit-testing silently wrong (a
   click landing on a window's real, drifted on-screen position would
   miss its NOMINAL rect entirely). Window 1 stays offset (+50, +50)
   from window 0, preserving the same genuine 150x100 overlap Milestone
   28 proved compositing correctness with. */
#define WINDOWS_TOTAL 2u
static const uint32_t window_x[WINDOWS_TOTAL] = { 100u, 150u };
static const uint32_t window_y[WINDOWS_TOTAL] = { 500u, 550u };

/* Milestone 30: each window's full state, kept alive for the server's
   ENTIRE (now persistent) lifetime -- Milestone 27/28's own server
   discarded `va` the moment a window was first presented, since it
   never needed to touch that window's pixels again. Recompositing on
   a raise needs every window's mapped buffer to still be valid. */
typedef struct {
    uint64_t x, y, w, h;
    uint64_t va;
} window_t;

static window_t windows[WINDOWS_TOTAL];
/* z_order[0] = bottom (drawn first), z_order[WINDOWS_TOTAL-1] = top
   (drawn last, wins any overlap). Values are indices into windows[].
   Starts as identity (window 0 bottom, window 1 top), matching the
   initial setup loop's own presentation order. */
static uint32_t z_order[WINDOWS_TOTAL];

static const char msg_acquired[] = "[OK] display server: framebuffer acquired\n";
static const char msg_acquire_failed[] = "[FAIL] display server: sys_fb_acquire failed\n";
static const char msg_reacquire_ok[] = "[OK] display server: a second sys_fb_acquire correctly failed (framebuffer already owned)\n";
static const char msg_reacquire_bug[] = "[FAIL] display server: a second sys_fb_acquire incorrectly succeeded\n";
static const char msg_subscribed[] = "[OK] display server: subscribed to hardware input events\n";
static const char msg_subscribe_failed[] = "[FAIL] display server: sys_input_subscribe failed\n";
static const char msg_resubscribe_ok[] = "[OK] display server: a second sys_input_subscribe correctly failed (already subscribed)\n";
static const char msg_resubscribe_bug[] = "[FAIL] display server: a second sys_input_subscribe incorrectly succeeded\n";
static const char msg_present_ok[] = "[OK] display server: presented window 0x";
static const char msg_present_failed[] = "[FAIL] display server: mapping or presenting a client's buffer failed\n";
static const char msg_all_ok[] = "[OK] display server: all windows presented in z-order\n";
static const char msg_raised[] = "[OK] display server: raised window 0x";

static void composite_all(void)
{
    for (uint32_t i = 0; i < WINDOWS_TOTAL; i++) {
        window_t *win = &windows[z_order[i]];
        sys_fb_present(win->x, win->y, win->w, win->h, (const void *)(uintptr_t)win->va);
    }
}

/* Returns the z_order POSITION (0..WINDOWS_TOTAL-1) of the topmost
   window whose rect contains (px, py), scanning from the top down --
   the real hit-testing convention (whichever window is drawn LAST
   visually wins any point both windows happen to cover). Returns -1 if
   the point is over the bare background (no window there at all). */
static int hit_test(uint64_t px, uint64_t py)
{
    for (uint32_t pos = WINDOWS_TOTAL; pos-- > 0;) {
        window_t *win = &windows[z_order[pos]];
        if (px >= win->x && px < win->x + win->w && py >= win->y && py < win->y + win->h) {
            return (int)pos;
        }
    }
    return -1;
}

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

    /* Milestone 30: subscribes to input BEFORE serving either client --
       this server is now the desktop's own real input consumer,
       superseding Milestone 29's standalone proof-of-mechanism demo
       (retired this same milestone; see ADR 0030's Decision). Same
       kernel-enforced exclusivity self-check shape as sys_fb_acquire()
       above. */
    if (sys_input_subscribe() == (uint64_t)-1) {
        sys_write(msg_subscribe_failed, sizeof(msg_subscribe_failed) - 1);
        return 1;
    }
    sys_write(msg_subscribed, sizeof(msg_subscribed) - 1);

    if (sys_input_subscribe() != (uint64_t)-1) {
        sys_write(msg_resubscribe_bug, sizeof(msg_resubscribe_bug) - 1);
        return 1;
    }
    sys_write(msg_resubscribe_ok, sizeof(msg_resubscribe_ok) - 1);

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

        windows[i].x = x;
        windows[i].y = y;
        windows[i].w = granted_w;
        windows[i].h = granted_h;
        windows[i].va = va;
        z_order[i] = i;

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

    /* Milestone 30: the persistent event loop -- this server never
       returns from here during a normal boot. Created BEFORE
       kernel_main's own frame-leak baseline (kernel/kernel.c), exactly
       like Milestone 29's retired standalone demo was, so neither that
       baseline nor the reap-count self-test gate every other test
       depends on ever waits for this process to exit. */
    for (;;) {
        ipc_message_t msg;
        sys_ipc_recv(&msg);

        if (msg.fields[0] != INPUT_EVENT_CLICK) {
            continue; /* not a message shape this milestone understands yet -- see Known limitations */
        }

        uint64_t click_x = msg.fields[1];
        uint64_t click_y = msg.fields[2];
        int hit_pos = hit_test(click_x, click_y);
        if (hit_pos < 0 || (uint32_t)hit_pos == WINDOWS_TOTAL - 1) {
            continue; /* background, or already the topmost window -- nothing to do */
        }

        /* WINDOWS_TOTAL is fixed at 2, so "raise the window at
           position 0" always means "swap with position 1" -- written
           this way (rather than a general shift loop) since a loop
           would never actually exercise more than this one case,
           matching CLAUDE.md's "don't build machinery you don't need"
           stance for a fixed, small N. */
        uint32_t raised = z_order[0];
        z_order[0] = z_order[1];
        z_order[1] = raised;

        composite_all();

        sys_write(msg_raised, sizeof(msg_raised) - 1);
        char digit = (char)('0' + raised);
        sys_write(&digit, 1);
        sys_write("\n", 1);
    }
}
