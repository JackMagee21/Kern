/* Milestone 27 (ADR 0027) / Milestone 28 (ADR 0028) / Milestone 30
   (ADR 0030) / Milestone 31 (ADR 0031): the piece of Desktop.md's GUI
   arc that actually owns pixels on screen. Milestone 27 proved the
   client-server model works at all with exactly one client; Milestone
   28 extended this to WINDOWS_TOTAL (2) clients with real spatial
   overlap; Milestone 30 made the server persistent and wired real
   clicks into raising a window; Milestone 31 adds the first real
   window CHROME (a title bar drawn by the server itself, never the
   client) plus drag-to-move and a close button -- this is where it
   stops being a windowing demo and starts being something a person
   sitting at the keyboard can actually drag around.

   Claims sole ownership of the real framebuffer via sys_fb_acquire()
   (kernel-ENFORCED, Milestone 27), subscribes to real input
   (Milestone 29/30), then serves each client in turn over the tiny
   request/grant/present protocol (display_protocol.h): grants a canvas
   no larger than its own fixed maximum -- "the server enforces the
   bound" -- at a cascaded position, composites it plus a title bar
   ABOVE it (Milestone 31's own addition -- the client never knows or
   draws its own chrome; this server owns that entirely, a deliberate
   split so a client's own bug or hostility can't fake window
   decorations), and ACKs the client before moving on. Serving the
   INITIAL setup in a strict sequential loop, and later recompositing
   in z-order after any raise/drag/close, both rely on the same fact:
   every window (chrome included) is fully OPAQUE, so painting
   bottom-to-top in z-order is sufficient, no separate damage-tracking
   pass needed (Milestone 28's own reasoning, still true here).

   The persistent event loop now understands THREE real input events
   (kernel/user/input_protocol.h): INPUT_EVENT_CLICK hit-tests the
   click against the CURRENT z-order (topmost first) -- on the title
   bar's close button, the window closes; elsewhere on the title bar,
   the window raises (if needed) AND a drag begins; on the canvas, the
   window just raises. INPUT_EVENT_DRAG (sent by the kernel only while
   the button is held and the cursor is moving) repositions whichever
   window a drag is in progress for, clamped to stay fully on screen
   and below the console's own reserved region (Milestone 30's own
   fix). INPUT_EVENT_RELEASE ends the drag.

   Milestone 33 (ADR 0033) adds the third window this server ever
   serves -- kernel/user/pulse_app.c, the first client in this whole
   project that keeps running indefinitely and keeps CHANGING what it
   draws, not just presenting once and exiting (clients A and B).
   DISPLAY_OP_REDRAW (display_protocol.h) is the new message this
   needs: a client that already completed its own REQUEST/PRESENT/ACK
   handshake, sent any number of further times to mean "recomposite
   everything -- I just wrote fresh pixels into the SAME shm buffer you
   already have mapped." Handling it needs no new per-window state at
   all: composite_all() already re-reads every window's stored `va`
   from scratch on every call, so whatever the client most recently
   wrote is picked up automatically.

   Milestone 34 (ADR 0034): closing a window now actually tells its
   owning client to exit (DISPLAY_OP_EXIT), rather than leaving it
   running forever with nothing displaying it -- the real gap Milestone
   33's own Known limitations flagged once the pulse app became the
   first client that could still be alive when its window closes.
   window_t gained a `pid` field (filled in once at grant time from the
   client's own DISPLAY_OP_REQUEST) so handle_click()'s close branch
   knows who to tell. */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"
#include "input_protocol.h"

/* This server's own fixed policy: never grant a client more canvas
   than this, no matter what it asks for. Placed here, not in
   display_protocol.h, since it's this SPECIFIC server's own decision,
   not part of the shared wire format -- a different server built later
   could reasonably choose a different maximum. Every chrome/clear
   buffer below is sized assuming every window is granted EXACTLY this
   width -- true for this demo's two clients (both request more, both
   get clamped to it) but not a general "any width" server. */
#define MAX_CANVAS_W 200u
#define MAX_CANVAS_H 150u

/* Milestone 31 (ADR 0031): the title bar's own height, and the close
   button's size/margin within it. */
#define CHROME_H 20u
#define CLOSE_BTN_SIZE 16u
#define CLOSE_BTN_MARGIN 2u

/* Milestone 30 (ADR 0030): duplicated from kernel/drivers/fbconsole.c's
   own FBCONSOLE_MAX_HEIGHT_PX -- the same "plain documented constant,
   both sides hardcode consistently" pattern already used for
   MAX_CANVAS_W/H's own value (this file's own doc comment) and for
   syscall numbers (ADR 0024). A window's title bar top must never be
   dragged above this value, or console text scrolling would shift it
   again -- the exact bug Milestone 30 fixed for the windows' INITIAL
   position; dragging must not be able to undo that fix. */
#define FBCONSOLE_MAX_HEIGHT_PX 480u

/* Milestone 30 (ADR 0030) / Milestone 33 (ADR 0033): a fixed cascade
   placement, one entry per client, in the exact order this server
   expects to serve them (client A first, client B second, the pulse
   app third -- enforced by the A->B->C go-signal chain, not assumed).
   Window 1 is offset (+50, +50) from window 0 so their granted
   200x150/200x150 canvases genuinely overlap (a 150x100 shared
   region) -- windows A/B's own exact-pixel test assertions
   (tests/qemu/test_display_server_selftest.sh, test_window_chrome_
   selftest.sh) depend on this pair's geometry exactly as before.
   Window 2 (the pulse app) is placed well clear of both -- x=650
   leaves a wide gap past window 1's own rightmost extent (150 + 200 =
   350), so it never overlaps A or B and can't perturb either test's
   pixel counts. y >= FBCONSOLE_MAX_HEIGHT_PX + CHROME_H so even each
   window's OWN title bar starts outside the console's reserved scroll
   region from the moment it's first drawn. */
#define WINDOWS_TOTAL 3u
static const uint32_t window_x[WINDOWS_TOTAL] = { 100u, 150u, 650u };
static const uint32_t window_y[WINDOWS_TOTAL] = { 500u, 550u, 520u };

/* Milestone 30: each window's full state, kept alive for the server's
   ENTIRE (persistent) lifetime -- recompositing on a raise/drag needs
   every window's mapped buffer to still be valid. Milestone 31 adds
   `closed`: a closed window is skipped by hit-testing and
   compositing, but its slot/buffer isn't reclaimed (no client
   teardown protocol exists for that yet -- see Known limitations). */
typedef struct {
    uint64_t x, y, w, h;
    uint64_t va;
    uint64_t pid; /* Milestone 34 (ADR 0034): the client that granted this window, so a close can tell it to actually exit -- see DISPLAY_OP_EXIT's own doc comment */
    int closed;
} window_t;

static window_t windows[WINDOWS_TOTAL];
/* z_order[0] = bottom (drawn first), z_order[WINDOWS_TOTAL-1] = top
   (drawn last, wins any overlap). Values are indices into windows[].
   Starts as identity (window 0 bottom, window 1 top), matching the
   initial setup loop's own presentation order. */
static uint32_t z_order[WINDOWS_TOTAL];

/* Milestone 31: -1 when no drag is in progress, else the index (into
   windows[], NOT a z_order position) of the window being dragged.
   drag_offset_x/y is the fixed vector from the window's own (x, y) to
   the point inside it that was actually clicked, captured once at
   drag-start, so the window doesn't "jump" to have its corner snap to
   the cursor on the very first drag step. */
static int dragging_window = -1;
static int64_t drag_offset_x;
static int64_t drag_offset_y;

static uint32_t screen_w;
static uint32_t screen_h;

/* Milestone 31: filled once at startup (init_chrome_buffer()) -- a
   solid title-bar color with a distinct close-button square baked in.
   Entirely server-owned; no client ever sees or influences this
   buffer. Sized for MAX_CANVAS_W's own stride (see that macro's doc
   comment on why every window here is assumed exactly that wide). */
static uint32_t chrome_buffer[MAX_CANVAS_W * CHROME_H];

/* Milestone 31: solid black (zero-initialized, .bss -- 0x00000000 is
   already "no color" in this protocol's plain 0x00RRGGBB convention,
   so no explicit fill is needed), used to erase a window's ENTIRE old
   footprint (title bar + canvas) before it moves or closes, so no
   stale pixels are ever left behind once the (possibly smaller,
   post-close) remaining set is recomposited on top. */
static const uint32_t clear_buffer[MAX_CANVAS_W * (MAX_CANVAS_H + CHROME_H)];

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
static const char msg_drag_start[] = "[OK] display server: started dragging window 0x";
static const char msg_dragged[] = "[OK] display server: dragged window 0x";
static const char msg_closed[] = "[OK] display server: closed window 0x";

static void init_chrome_buffer(void)
{
    uint32_t title_color = 0x00404040u; /* dark grey, r=64 g=64 b=64 */
    uint32_t close_color = 0x00FF00FFu; /* magenta, r=255 g=0 b=255 -- distinct from the cursor's red, both clients' colors, and the title bar's own grey */
    for (uint32_t i = 0; i < MAX_CANVAS_W * CHROME_H; i++) {
        chrome_buffer[i] = title_color;
    }
    uint32_t btn_x0 = MAX_CANVAS_W - CLOSE_BTN_MARGIN - CLOSE_BTN_SIZE;
    uint32_t btn_y0 = (CHROME_H - CLOSE_BTN_SIZE) / 2u;
    for (uint32_t y = 0; y < CLOSE_BTN_SIZE; y++) {
        for (uint32_t x = 0; x < CLOSE_BTN_SIZE; x++) {
            chrome_buffer[(btn_y0 + y) * MAX_CANVAS_W + (btn_x0 + x)] = close_color;
        }
    }
}

/* Presents a single window's title bar (if not closed) then its
   canvas, in that order (chrome first, then content -- both fully
   opaque, so the actual order between the two doesn't matter for
   correctness, only that both happen). */
static void present_window(const window_t *win)
{
    if (win->closed) {
        return;
    }
    sys_fb_present(win->x, win->y - CHROME_H, MAX_CANVAS_W, CHROME_H, chrome_buffer);
    sys_fb_present(win->x, win->y, win->w, win->h, (const void *)(uintptr_t)win->va);
}

static void composite_all(void)
{
    for (uint32_t i = 0; i < WINDOWS_TOTAL; i++) {
        present_window(&windows[z_order[i]]);
    }
}

/* Returns the z_order POSITION (0..WINDOWS_TOTAL-1) of the topmost
   OPEN window whose title bar, close button, or canvas contains
   (px, py), scanning from the top down -- the real hit-testing
   convention (whichever window is drawn LAST visually wins any point
   both windows happen to cover). *out_region is set to 2 (close
   button), 1 (title bar, elsewhere), or 0 (canvas). Returns -1 (region
   untouched) if the point is over the bare background or a closed
   window. */
static int hit_test(uint64_t px, uint64_t py, int *out_region)
{
    for (uint32_t pos = WINDOWS_TOTAL; pos-- > 0;) {
        const window_t *win = &windows[z_order[pos]];
        if (win->closed) {
            continue;
        }

        uint64_t btn_x0 = win->x + MAX_CANVAS_W - CLOSE_BTN_MARGIN - CLOSE_BTN_SIZE;
        uint64_t btn_y0 = win->y - CHROME_H + (CHROME_H - CLOSE_BTN_SIZE) / 2u;
        if (px >= btn_x0 && px < btn_x0 + CLOSE_BTN_SIZE && py >= btn_y0 && py < btn_y0 + CLOSE_BTN_SIZE) {
            *out_region = 2;
            return (int)pos;
        }
        if (px >= win->x && px < win->x + MAX_CANVAS_W && py >= win->y - CHROME_H && py < win->y) {
            *out_region = 1;
            return (int)pos;
        }
        if (px >= win->x && px < win->x + win->w && py >= win->y && py < win->y + win->h) {
            *out_region = 0;
            return (int)pos;
        }
    }
    return -1;
}

/* Milestone 33 (ADR 0033): a general shift, not a fixed 2-element
   swap -- WINDOWS_TOTAL is now 3, and a 2-element swap is simply wrong
   for raising a window out of the BOTTOM of a 3-deep stack (it would
   only ever exchange positions 0 and 1, never actually reaching the
   top). Shifts every window ABOVE hit_pos down by one slot, then
   places the raised window at the very top -- reduces to the exact
   same swap as before whenever hit_pos == 0 and WINDOWS_TOTAL == 2, so
   this isn't a behavior change for that case, just a correct
   generalization for N > 2. */
static void raise_to_top(int hit_pos)
{
    if ((uint32_t)hit_pos == WINDOWS_TOTAL - 1) {
        return; /* already topmost */
    }
    uint32_t raised = z_order[hit_pos];
    for (uint32_t i = (uint32_t)hit_pos; i < WINDOWS_TOTAL - 1; i++) {
        z_order[i] = z_order[i + 1];
    }
    z_order[WINDOWS_TOTAL - 1] = raised;
    composite_all();

    sys_write(msg_raised, sizeof(msg_raised) - 1);
    char digit = (char)('0' + z_order[WINDOWS_TOTAL - 1]);
    sys_write(&digit, 1);
    sys_write("\n", 1);
}

static void handle_click(uint64_t x, uint64_t y)
{
    int region;
    int hit_pos = hit_test(x, y, &region);
    if (hit_pos < 0) {
        return; /* background -- nothing to do */
    }
    uint32_t win_idx = z_order[hit_pos];
    window_t *win = &windows[win_idx];

    if (region == 2) {
        /* Close: erase this window's ENTIRE old footprint (title bar +
           canvas), mark it closed, then recomposite whatever's left --
           correctly redraws the OTHER window if it was partially
           covered by the one that just closed, the same "opaque
           windows, paint bottom-to-top" reasoning composite_all()
           already relies on everywhere else. */
        sys_fb_present(win->x, win->y - CHROME_H, MAX_CANVAS_W, win->h + CHROME_H, clear_buffer);
        win->closed = 1;
        if (dragging_window == (int)win_idx) {
            dragging_window = -1;
        }
        composite_all();

        /* Milestone 34 (ADR 0034): tell the client that owned this
           window to actually exit -- see DISPLAY_OP_EXIT's own doc
           comment (display_protocol.h) for why this is always safe to
           send even to a client that already exited on its own
           (clients A/B). Sent AFTER composite_all(), not before --
           this window is already gone from the screen and its own
           closed flag already set, so even if the client is still
           alive and could somehow act instantly, there's no ordering
           hazard either way. */
        ipc_message_t exit_msg = { .fields = { DISPLAY_OP_EXIT, 0, 0, 0 } };
        sys_ipc_send(win->pid, &exit_msg);

        sys_write(msg_closed, sizeof(msg_closed) - 1);
        char digit = (char)('0' + win_idx);
        sys_write(&digit, 1);
        sys_write("\n", 1);
        return;
    }

    raise_to_top(hit_pos);

    if (region == 1) {
        dragging_window = (int)win_idx;
        drag_offset_x = (int64_t)x - (int64_t)win->x;
        drag_offset_y = (int64_t)y - (int64_t)win->y;

        sys_write(msg_drag_start, sizeof(msg_drag_start) - 1);
        char digit = (char)('0' + win_idx);
        sys_write(&digit, 1);
        sys_write("\n", 1);
    }
}

static void handle_drag(uint64_t x, uint64_t y)
{
    if (dragging_window < 0) {
        return; /* a drag-move arrived with no active drag (e.g. this server just started) -- ignore */
    }
    window_t *win = &windows[dragging_window];

    int64_t new_x = (int64_t)x - drag_offset_x;
    int64_t new_y = (int64_t)y - drag_offset_y;

    int64_t min_y = (int64_t)(FBCONSOLE_MAX_HEIGHT_PX + CHROME_H);
    if (new_x < 0) {
        new_x = 0;
    }
    if ((uint64_t)new_x + MAX_CANVAS_W > screen_w) {
        new_x = (int64_t)(screen_w - MAX_CANVAS_W);
    }
    if (new_y < min_y) {
        new_y = min_y;
    }
    if ((uint64_t)new_y + win->h > screen_h) {
        new_y = (int64_t)(screen_h - win->h);
    }

    sys_fb_present(win->x, win->y - CHROME_H, MAX_CANVAS_W, win->h + CHROME_H, clear_buffer);
    win->x = (uint64_t)new_x;
    win->y = (uint64_t)new_y;
    composite_all();

    sys_write(msg_dragged, sizeof(msg_dragged) - 1);
    char digit = (char)('0' + dragging_window);
    sys_write(&digit, 1);
    sys_write("\n", 1);
}

int main(void)
{
    uint64_t fb_info = sys_fb_acquire();
    if (fb_info == (uint64_t)-1) {
        sys_write(msg_acquire_failed, sizeof(msg_acquire_failed) - 1);
        return 1;
    }
    /* Milestone 27's own acquire also returns the negotiated screen
       dimensions, packed (width << 32) | height -- Milestone 31 is
       this server's first real use of them (clamping a drag so a
       window can never be dragged off screen). */
    screen_w = (uint32_t)(fb_info >> 32);
    screen_h = (uint32_t)(fb_info & 0xffffffffu);
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
       (retired that same milestone). Same kernel-enforced exclusivity
       self-check shape as sys_fb_acquire() above. */
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

    init_chrome_buffer();

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
        windows[i].pid = req.sender_pid; /* Milestone 34: remembered for DISPLAY_OP_EXIT on close */
        windows[i].closed = 0;
        z_order[i] = i;

        present_window(&windows[i]);

        /* Only sent once present_window() above has actually returned
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

    /* Milestone 30/31: the persistent event loop -- this server never
       returns from here during a normal boot. Created BEFORE
       kernel_main's own frame-leak baseline (kernel/kernel.c), so
       neither that baseline nor the reap-count self-test gate every
       other test depends on ever waits for this process to exit. */
    for (;;) {
        ipc_message_t msg;
        sys_ipc_recv(&msg);

        uint64_t x = msg.fields[1];
        uint64_t y = msg.fields[2];
        switch (msg.fields[0]) {
        case INPUT_EVENT_CLICK:
            handle_click(x, y);
            break;
        case INPUT_EVENT_DRAG:
            handle_drag(x, y);
            break;
        case INPUT_EVENT_RELEASE:
            dragging_window = -1;
            break;
        case DISPLAY_OP_REDRAW:
            /* Milestone 33: a client (the pulse app) wrote fresh
               pixels into its ALREADY-mapped shm buffer and wants a
               recomposite -- no per-window lookup needed, see this
               file's own top-of-file comment for why. */
            composite_all();
            break;
        default:
            break; /* not a message shape this server understands yet */
        }
    }
}
