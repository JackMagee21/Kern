/* Milestone 33 (ADR 0033): the third window kernel/user/display_server.c
   serves, and the first client in this whole project that keeps
   running -- and keeps CHANGING what it draws -- indefinitely, instead
   of presenting exactly once and exiting (clients A and B,
   display_client_a.c/display_client_b.c). Desktop.md's final GUI-arc
   item asks for "something interactive enough to prove input routing
   works end to end"; this proves the complementary half -- that a real
   application can stay alive and keep updating its own window on
   screen, which the server-driven click/drag/close machinery (Milestone
   29-31) never had to exercise before now, since every prior client's
   canvas was fixed for its whole (brief) lifetime.

   Deliberately visually minimal -- a solid color that cycles through a
   small fixed palette -- to avoid needing text/font rendering as a new
   subsystem just to prove this. A real clock (reading actual wall-clock
   time) is reasonable future work once a time-reading syscall exists
   (kernel/drivers/rtc.c's rtc_read() is kernel-only today, no sys_*
   wrapper yet); this app paces itself with a plain sys_nop spin, the
   same bounded-loop-for-determinism pattern kernel/user/fork_demo.asm
   already established, not a real clock tick.

   Joins the SAME A -> B -> C go-signal chain client A started
   (Milestone 28, ADR 0028): after client B's own canvas has landed, B
   signals this process, so this process's own DISPLAY_OP_REQUEST is
   guaranteed to reach the server third, in the exact order the
   server's initial setup loop (display_server.c's own main()) expects
   window_x[2]/window_y[2] to apply to.

   Milestone 34 (ADR 0034): this process CAN now exit -- once per
   frame it polls (sys_ipc_try_recv(), never blocking) for a server-
   sent DISPLAY_OP_EXIT, the real fix for the gap Milestone 33's own
   Known limitations flagged: closing this window used to leave this
   process spinning forever with nothing displaying it. */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"

#define REQUESTED_W 150u
#define REQUESTED_H 100u

/* Milestone 33: how many sys_nop iterations pass between animation
   frames -- generous, matching fork_demo.asm's own 200000-iteration
   precedent, so a boot's exact frame-rate doesn't depend on host/QEMU
   speed for correctness (only for how MANY frames happen to land
   before someone looks, never whether the mechanism itself works). */
#define FRAME_DELAY_NOPS 200000u

/* A small fixed palette, each entry a distinctive 0x00RRGGBB color --
   far from black, clients A/B's own teal/orange, and the chrome's
   magenta close button. NOT a pure/near-pure red (unlike an earlier
   version of this palette): kernel/drivers/cursor.c's own cursor is
   solid red (0xff, 0x00, 0x00), and tests/qemu/test_framebuffer_
   selftest.sh's cursor-position check scans the WHOLE SCREEN with a
   generously loose matcher (r>200, g<60, b<60) built back when only an
   8x8 sprite could ever be that shade -- this window is 150x100, big
   enough that a near-red entry here would get merged into that same
   scan and silently break that test's bounding box. Found by actually
   running the full regression suite, not assumed safe in advance
   (CLAUDE.md: "actually run the...test and show its output"). Purple
   was picked specifically because it fails EVERY existing color
   matcher in every tests/qemu smoke test by construction (checked
   directly, not guessed) -- green/blue/yellow were already distinct
   enough to need no change. */
static const uint32_t palette[] = { 0x00CC33FFu, 0x0033FF33u, 0x003333FFu, 0x00FFFF33u };
#define PALETTE_LEN (sizeof(palette) / sizeof(palette[0]))

static const char msg_ok[] = "[OK] pulse app: canvas presented via the display server, now animating\n";
static const char msg_bad[] = "[FAIL] pulse app: display protocol handshake failed\n";
static const char msg_go_bad[] = "[FAIL] pulse app: go-signal from client B had the wrong opcode\n";
static const char msg_exit[] = "[OK] pulse app: received exit request, exiting\n";

int main(void)
{
    ipc_message_t boot;
    sys_ipc_recv(&boot); /* kernel_main's own bootstrap message: fields[0] = the display server's pid */
    uint64_t server_pid = boot.fields[0];

    ipc_message_t go;
    sys_ipc_recv(&go); /* blocks until client B's DISPLAY_OP_GO arrives -- both A and B's windows are guaranteed already on screen by then */
    if (go.fields[0] != DISPLAY_OP_GO) {
        sys_write(msg_go_bad, sizeof(msg_go_bad) - 1);
        return 1;
    }

    ipc_message_t req = { .fields = { DISPLAY_OP_REQUEST, REQUESTED_W, REQUESTED_H, 0 } };
    if (sys_ipc_send(server_pid, &req) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    ipc_message_t grant;
    sys_ipc_recv(&grant);
    uint64_t granted_w = grant.fields[3] >> 32;
    uint64_t granted_h = grant.fields[3] & 0xffffffffu;
    if (granted_w == 0 || granted_h == 0) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    uint64_t shm_id = sys_shm_create(granted_w * granted_h * 4);
    uint64_t va = shm_id != 0 ? sys_shm_map(shm_id) : 0;
    if (va == 0) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    uint32_t *buf = (uint32_t *)(uintptr_t)va;
    uint64_t pixel_count = granted_w * granted_h;
    uint32_t color_index = 0;
    for (uint64_t i = 0; i < pixel_count; i++) {
        buf[i] = palette[color_index];
    }

    ipc_message_t pres = { .fields = { DISPLAY_OP_PRESENT, shm_id, 0, 0 } };
    if (sys_ipc_send(server_pid, &pres) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    ipc_message_t ack;
    sys_ipc_recv(&ack); /* waits for the server's own confirmation this canvas actually landed -- see display_client_a.c's identical comment */

    sys_write(msg_ok, sizeof(msg_ok) - 1);

    /* Milestone 33: unlike every earlier client, this process never
       exits on its own -- it keeps rewriting its OWN already-mapped
       canvas (sys_shm_map() is never repeated, only the pixel CONTENTS
       change) and pinging the server with DISPLAY_OP_REDRAW so the new
       content actually reaches the screen. Created in kernel.c's
       "permanent process" zone (before the frame-leak baseline), the
       same treatment display_server.c's own persistent process gets,
       since this loop was designed to never return on its own.

       Milestone 34 (ADR 0034): "on its own" now has an exception --
       once per frame (not every sys_nop, which would turn a cheap poll
       into the dominant cost of this loop) this checks, with the
       NON-blocking sys_ipc_try_recv(), whether the server sent
       DISPLAY_OP_EXIT (its window was closed). Using sys_ipc_recv here
       instead would mean blocking forever the moment a close arrives
       is genuinely fine, but blocking forever WAITING for one that may
       never come would kill this app's own animation -- exactly what
       sys_ipc_try_recv (kernel/arch/x86_64/syscall.c) exists for. */
    for (;;) {
        for (uint64_t spin = 0; spin < FRAME_DELAY_NOPS; spin++) {
            sys_nop();
        }

        ipc_message_t msg;
        if (sys_ipc_try_recv(&msg) == 0 && msg.fields[0] == DISPLAY_OP_EXIT) {
            sys_write(msg_exit, sizeof(msg_exit) - 1);
            sys_exit(0);
        }

        color_index = (color_index + 1) % PALETTE_LEN;
        for (uint64_t i = 0; i < pixel_count; i++) {
            buf[i] = palette[color_index];
        }
        ipc_message_t redraw = { .fields = { DISPLAY_OP_REDRAW, 0, 0, 0 } };
        sys_ipc_send(server_pid, &redraw);
    }
}
