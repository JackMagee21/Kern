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
   knows who to tell.

   Milestone 35 (ADR 0035): a fourth window, kernel/user/clock_app.c --
   a real clock, reading actual wall-clock time via the new
   sys_rtc_read() syscall. WINDOWS_TOTAL raised 3 -> 4; needed no other
   change here at all, since raise_to_top()'s general shift loop
   (Milestone 33) and composite_all()'s own "read every window's
   current `va`" design already generalize to any WINDOWS_TOTAL. */

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

/* Milestone 36 (ADR 0036): placement for a DYNAMICALLY spawned window
   (handle_dynamic_request(), below) -- deliberately simpler than the
   boot-time window_x[]/window_y[] table, since a spawned window's
   exact position was never load-bearing for any test's exact-pixel
   assertions (unlike the boot-time four). Cascades diagonally so
   several spawns in a row are still individually clickable rather than
   stacking exactly on top of each other; wraps back to the base point
   every DYNAMIC_CASCADE_SLOTS spawns rather than walking off-screen.

   Placed in the genuinely EMPTY horizontal gap between client B's own
   right edge (150 + 200 = 350) and the pulse/clock apps' own left edge
   (650) -- x=380 .. x=380+3*10+200=610 (all four cascade slots) stays
   entirely within that gap, so a dynamically spawned window never
   overlaps ANY boot-time window. This is NOT because overlapping
   compositing is broken (it isn't -- z-order painting genuinely
   handles it, and dragging a window into an overlap already proves
   this, Milestone 31): investigating an early version of this
   milestone that DID place spawned windows over client B found that
   QEMU's own `screendump` monitor command can show STALE pixel data
   for a region a headless "-display none" session hasn't actively
   redrawn in a while, even though a kernel-side readback
   (fb_read_rect(), kernel/drivers/framebuffer.c) taken immediately
   after the exact same write confirmed the real framebuffer memory was
   already correct -- a QEMU/test-harness display-refresh artifact, not
   a kernel bug (verified directly, not assumed: CLAUDE.md's "diagnose
   first" discipline applied all the way to the actual root cause
   before deciding this wasn't worth chasing further upstream). Picking
   a non-overlapping default position sidesteps needing to work around
   that QEMU-side artifact in every future screendump-based smoke test,
   without giving up anything this milestone actually needs to prove. */
#define DYNAMIC_BASE_X 380u
#define DYNAMIC_BASE_Y 560u
#define DYNAMIC_CASCADE_STEP_X 10u
#define DYNAMIC_CASCADE_STEP_Y 10u
#define DYNAMIC_CASCADE_SLOTS 4u

/* Milestone 30 (ADR 0030) / Milestone 33 (ADR 0033) / Milestone 35
   (ADR 0035): a fixed cascade placement, one entry per client THIS
   SERVER SERVES AT BOOT, in the exact order it expects to serve them
   (client A first, client B second, the pulse app third, the clock
   app fourth -- enforced by the A->B->C->D go-signal chain, not
   assumed). Window 1 is offset (+50, +50) from window 0 so their
   granted 200x150/200x150 canvases genuinely overlap (a 150x100
   shared region) -- windows A/B's own exact-pixel test assertions
   (tests/qemu/test_display_server_selftest.sh, test_window_chrome_
   selftest.sh) depend on this pair's geometry exactly as before.
   Window 2 (the pulse app) is placed well clear of both -- x=650
   leaves a wide gap past window 1's own rightmost extent (150 + 200 =
   350), so it never overlaps A or B and can't perturb either test's
   pixel counts. Window 3 (the clock app) sits directly below window 2
   at the SAME x (650) but y=700 -- window 2's own chrome+canvas
   footprint ends at y=620 (520 + 100), well clear of window 3's own
   chrome starting at 680 (700 - CHROME_H), and 700 + 50 = 750 stays
   within the negotiated 768px screen height. y >= FBCONSOLE_MAX_HEIGHT_PX
   + CHROME_H (windows 0-2) so even each window's OWN title bar starts
   outside the console's reserved scroll region from the moment it's
   first drawn -- window 3 clears this by construction too (700 > 480). */
#define WINDOWS_BOOT_COUNT 4u
static const uint32_t window_x[WINDOWS_BOOT_COUNT] = { 100u, 150u, 650u, 650u };
static const uint32_t window_y[WINDOWS_BOOT_COUNT] = { 500u, 550u, 520u, 700u };

/* Milestone 36 (ADR 0036): a FIXED-CAPACITY array, not a general
   dynamic list -- the same "bounded array, scanned/managed, sized
   generously for this hobby kernel's scale" pattern
   kernel/sched/scheduler.c's own MAX_LIVE_TASKS registry and
   kernel/drivers/mouse.c's own event queues already established, not a
   new one invented here. WINDOWS_BOOT_COUNT (4) windows are always
   created during boot's own deterministic setup loop below;
   WINDOWS_CAPACITY - WINDOWS_BOOT_COUNT (4 more) are available for
   `shell spawn`-launched windows (kernel/shell.c) requested later, at
   any point, from this server's own persistent event loop. */
#define WINDOWS_CAPACITY 8u

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

static window_t windows[WINDOWS_CAPACITY];
/* z_order[0] = bottom (drawn first), z_order[windows_used-1] = top
   (drawn last, wins any overlap). Values are indices into windows[].
   Starts as identity (window 0 bottom, window 1 top, ...) for the
   WINDOWS_BOOT_COUNT boot-time windows, matching the initial setup
   loop's own presentation order; a dynamically spawned window
   (Milestone 36) is always appended at the current top. */
static uint32_t z_order[WINDOWS_CAPACITY];

/* Milestone 36 (ADR 0036): how many of windows[]/z_order[]'s
   WINDOWS_CAPACITY slots are actually in use right now -- starts at
   WINDOWS_BOOT_COUNT once the initial setup loop completes, and grows
   by one for each successful dynamic spawn (handle_dynamic_request()).
   Every function that used to loop `0..WINDOWS_TOTAL` (composite_all(),
   hit_test(), raise_to_top()) now loops `0..windows_used` instead --
   this is the ONLY change any of those three needed, since none of
   them ever assumed a compile-time-fixed bound beyond that loop
   itself. */
static uint32_t windows_used;

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
static const char msg_spawn_ok[] = "[OK] display server: dynamically presented window 0x";
static const char msg_spawn_full[] = "[FAIL] display server: dynamic spawn rejected (WINDOWS_CAPACITY reached)\n";
static const char msg_spawn_failed[] = "[FAIL] display server: mapping a dynamically spawned client's buffer failed\n";

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
    for (uint32_t i = 0; i < windows_used; i++) {
        present_window(&windows[z_order[i]]);
    }
}

/* Returns the z_order POSITION (0..windows_used-1) of the topmost
   OPEN window whose title bar, close button, or canvas contains
   (px, py), scanning from the top down -- the real hit-testing
   convention (whichever window is drawn LAST visually wins any point
   both windows happen to cover). *out_region is set to 2 (close
   button), 1 (title bar, elsewhere), or 0 (canvas). Returns -1 (region
   untouched) if the point is over the bare background or a closed
   window. */
static int hit_test(uint64_t px, uint64_t py, int *out_region)
{
    for (uint32_t pos = windows_used; pos-- > 0;) {
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
   swap -- a 2-element swap is simply wrong for raising a window out of
   the BOTTOM of a deeper stack (it would only ever exchange positions
   0 and 1, never actually reaching the top). Shifts every window
   ABOVE hit_pos down by one slot, then places the raised window at the
   very top -- reduces to the exact same swap as before whenever
   hit_pos == 0 and windows_used == 2. Already generalized to any
   window COUNT at Milestone 33; Milestone 36 only changes WHERE that
   count comes from (windows_used, a runtime variable that can now grow
   after a dynamic spawn, not the compile-time WINDOWS_TOTAL this
   function used to read). */
static void raise_to_top(int hit_pos)
{
    if ((uint32_t)hit_pos == windows_used - 1) {
        return; /* already topmost */
    }
    uint32_t raised = z_order[hit_pos];
    for (uint32_t i = (uint32_t)hit_pos; i < windows_used - 1; i++) {
        z_order[i] = z_order[i + 1];
    }
    z_order[windows_used - 1] = raised;
    composite_all();

    sys_write(msg_raised, sizeof(msg_raised) - 1);
    char digit = (char)('0' + z_order[windows_used - 1]);
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

static void dispatch_message(const ipc_message_t *msg); /* forward decl: handle_dynamic_request() below needs to call this before its own definition later in this file */

/* Milestone 38 (ADR 0038): the boot-time setup loop (main(), below)
   used to trust that the NEXT message in its own inbox was always
   exactly the REQUEST/PRESENT it was waiting for -- true only as long
   as nothing else could ever message the server during boot's own
   setup window. That stopped being safe once a persistent client (the
   pulse app, Milestone 33) could start sending its own background
   DISPLAY_OP_REDRAW pings WHILE a LATER client (the clock app,
   Milestone 35) was still completing ITS OWN handshake -- a real race
   TCG's much slower execution happened to never expose (pulse app's
   own 200000-sys_nop pacing spin takes long enough, in real wall-clock
   terms, under software emulation, that the whole 4-window boot
   sequence always finished first), but real KVM's hardware-
   accelerated speed genuinely CAN and DID trigger on a real boot (not
   guessed -- a stray REDRAW ping got misread as the clock app's own
   PRESENT, its shm_id field misread from what was actually a garbage
   position in an unrelated message, corrupting the whole handshake --
   found via a message-flow trace across kernel/ipc/msgqueue.c, not
   assumed). Fixed the exact same way handle_dynamic_request()'s own
   wait loop already handles this class of problem: keep processing
   anything that ISN'T the specific expected message via
   dispatch_message(), instead of trusting the next message blindly.
   Safe by the SAME reasoning already established there: the go-signal
   chain guarantees REQUESTs only ever arrive in the correct order, so
   recv_boot_request()'s own filter always accepts the very first
   message it sees; recv_boot_present()'s filter additionally checks
   sender_pid, so it can never be fooled by a stray PRESENT-shaped
   message from an unrelated client (dispatch_message()'s own default
   case silently drops anything it doesn't recognize, the same
   established "unknown message = drop" convention this protocol
   already uses everywhere else). */
static void recv_boot_request(ipc_message_t *out)
{
    for (;;) {
        sys_ipc_recv(out);
        if (out->fields[0] == DISPLAY_OP_REQUEST) {
            return;
        }
        dispatch_message(out);
    }
}

static void recv_boot_present(uint64_t expected_sender_pid, ipc_message_t *out)
{
    for (;;) {
        sys_ipc_recv(out);
        if (out->fields[0] == DISPLAY_OP_PRESENT && out->sender_pid == expected_sender_pid) {
            return;
        }
        dispatch_message(out);
    }
}

/* Milestone 36 (ADR 0036): the SAME REQUEST/GRANT/PRESENT/ACK
   handshake the boot-time setup loop (main(), below) already performs
   for windows 0-3 -- but reachable from the persistent event loop for
   a client that shows up AFTER boot, at any time (kernel/shell.c's
   `spawn` command). Grants (0, 0) -- an existing, already-handled
   failure shape every client's own handshake already checks for
   (`if (granted_w == 0 || granted_h == 0) { fail }`, e.g.
   pulse_app.c/clock_app.c) -- if WINDOWS_CAPACITY is already reached,
   needing no new client-side logic at all. */
static void handle_dynamic_request(const ipc_message_t *req)
{
    if (windows_used >= WINDOWS_CAPACITY) {
        ipc_message_t grant = { .fields = { DISPLAY_OP_GRANT, 0, 0, 0 } };
        sys_ipc_send(req->sender_pid, &grant);
        sys_write(msg_spawn_full, sizeof(msg_spawn_full) - 1);
        return;
    }

    uint32_t idx = windows_used;
    uint64_t requested_w = req->fields[1];
    uint64_t requested_h = req->fields[2];
    uint64_t granted_w = requested_w < MAX_CANVAS_W ? requested_w : MAX_CANVAS_W;
    uint64_t granted_h = requested_h < MAX_CANVAS_H ? requested_h : MAX_CANVAS_H;

    uint32_t slot = (idx - WINDOWS_BOOT_COUNT) % DYNAMIC_CASCADE_SLOTS;
    uint64_t x = DYNAMIC_BASE_X + slot * DYNAMIC_CASCADE_STEP_X;
    uint64_t y = DYNAMIC_BASE_Y + slot * DYNAMIC_CASCADE_STEP_Y;

    ipc_message_t grant = { .fields = { DISPLAY_OP_GRANT, x, y, (granted_w << 32) | granted_h } };
    sys_ipc_send(req->sender_pid, &grant);

    /* Waits specifically for THIS client's own DISPLAY_OP_PRESENT --
       but, unlike the boot-time setup loop (which runs before input is
       even enabled, kernel/kernel.c's own ordering), input IS already
       flowing by the time a dynamic spawn can happen. A mouse click on
       an existing window could genuinely arrive in this exact window,
       and popping it here by mistake (treating its fields as a bogus
       shm id) would corrupt state. dispatch_message() (below) handles
       any message that ISN'T the expected PRESENT exactly the way the
       main loop already would, then keeps waiting -- the same
       "process everything else normally while still waiting for one
       specific reply" pattern, not a new one. */
    ipc_message_t pres;
    for (;;) {
        sys_ipc_recv(&pres);
        if (pres.fields[0] == DISPLAY_OP_PRESENT && pres.sender_pid == req->sender_pid) {
            break;
        }
        dispatch_message(&pres);
    }

    uint64_t shm_id = pres.fields[1];
    uint64_t va = sys_shm_map(shm_id);
    if (va == 0) {
        sys_write(msg_spawn_failed, sizeof(msg_spawn_failed) - 1);
        return; /* malformed client -- drop it silently rather than risk the server itself */
    }

    windows[idx].x = x;
    windows[idx].y = y;
    windows[idx].w = granted_w;
    windows[idx].h = granted_h;
    windows[idx].va = va;
    windows[idx].pid = req->sender_pid;
    windows[idx].closed = 0;
    z_order[windows_used] = idx;
    windows_used++;

    present_window(&windows[idx]);

    ipc_message_t ack = { .fields = { DISPLAY_OP_ACK, 0, 0, 0 } };
    sys_ipc_send(req->sender_pid, &ack);

    sys_write(msg_spawn_ok, sizeof(msg_spawn_ok) - 1);
    char idx_digit = (char)('0' + idx);
    sys_write(&idx_digit, 1);
    sys_write("\n", 1);
}

/* Milestone 36 (ADR 0036): factored out of what used to be the
   persistent event loop's own inline switch, so handle_dynamic_
   request()'s own "keep processing everything else while waiting for
   one specific reply" sub-loop (above) can reuse the EXACT same
   dispatch, not a parallel copy that could silently drift out of
   sync.

   Dispatches on sender_pid FIRST, then on the opcode -- found (in
   review, before ever booting) that INPUT_EVENT_CLICK/DRAG/RELEASE
   (input_protocol.h: 1/2/3) and DISPLAY_OP_REQUEST/GRANT/PRESENT
   (display_protocol.h: 1/2/3) are two INDEPENDENTLY numbered
   protocols that happen to share the exact same low opcode values --
   never a problem before this milestone, since DISPLAY_OP_REQUEST was
   only ever consumed by main()'s own dedicated boot-time loop
   (running before input is even enabled), never this switch. Now that
   handle_dynamic_request() needs DISPLAY_OP_REQUEST reachable from
   HERE, a plain single switch on fields[0] would silently treat a
   spawned client's REQUEST (opcode 1) as an INPUT_EVENT_CLICK (also
   opcode 1). Fixed using an existing, verified invariant (read
   directly from the source, not assumed): kernel/drivers/
   input_router.c's own input_router_notify() calls ipc_send()
   directly (never sys_ipc_send()), so its message's sender_pid is left
   at its struct-literal default of 0 (ipc_message.h's own doc comment:
   sender_pid is normally filled in by sys_ipc_send() alone) -- and no
   real ring-3 client can ever legitimately have pid 0 (task ids start
   at 1, scheduler.c). sender_pid == 0 is therefore a safe, load-
   bearing way to tell "this is a kernel-originated input event" from
   "this is a real client's display-protocol message", not a guess. */
static void dispatch_message(const ipc_message_t *msg)
{
    if (msg->sender_pid == 0) {
        uint64_t x = msg->fields[1];
        uint64_t y = msg->fields[2];
        switch (msg->fields[0]) {
        case INPUT_EVENT_CLICK:
            handle_click(x, y);
            break;
        case INPUT_EVENT_DRAG:
            handle_drag(x, y);
            break;
        case INPUT_EVENT_RELEASE:
            dragging_window = -1;
            break;
        default:
            break; /* not a kernel-originated event shape this server understands yet */
        }
        return;
    }

    switch (msg->fields[0]) {
    case DISPLAY_OP_REDRAW:
        /* Milestone 33: a client (the pulse app or clock app) wrote
           fresh pixels into its ALREADY-mapped shm buffer and wants a
           recomposite -- no per-window lookup needed, see this file's
           own top-of-file comment for why. */
        composite_all();
        break;
    case DISPLAY_OP_REQUEST:
        /* Milestone 36: a client the boot-time setup loop never knew
           about -- kernel/shell.c's `spawn` command launched it after
           boot. */
        handle_dynamic_request(msg);
        break;
    default:
        break; /* not a client message shape this server understands yet */
    }
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

    for (uint32_t i = 0; i < WINDOWS_BOOT_COUNT; i++) {
        ipc_message_t req;
        recv_boot_request(&req); /* blocks until this window's DISPLAY_OP_REQUEST arrives -- processing anything else that arrives first, see this function's own doc comment */

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
        recv_boot_present(req.sender_pid, &pres); /* blocks until THIS SAME client's DISPLAY_OP_PRESENT arrives -- processing anything else (e.g. an already-onboarded persistent client's own background redraw) that arrives first, see recv_boot_present()'s own doc comment */

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

    windows_used = WINDOWS_BOOT_COUNT; /* Milestone 36: from here on, every remaining slot is available for a dynamic spawn */

    sys_write(msg_all_ok, sizeof(msg_all_ok) - 1);

    /* Milestone 30/31: the persistent event loop -- this server never
       returns from here during a normal boot. Created BEFORE
       kernel_main's own frame-leak baseline (kernel/kernel.c), so
       neither that baseline nor the reap-count self-test gate every
       other test depends on ever waits for this process to exit. */
    for (;;) {
        ipc_message_t msg;
        sys_ipc_recv(&msg);
        dispatch_message(&msg);
    }
}
