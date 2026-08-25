/* Milestone 28 (ADR 0028): the FIRST of two clients
   kernel/user/display_server.c now serves, proving multiple windows
   composite with a correct, deterministic z-order (Desktop.md's
   milestone 5's first slice -- real input-driven focus/raising is
   deliberately deferred to its own later milestone, see ADR 0028's
   Decision). Otherwise unchanged from Milestone 27's own single client
   (kernel/user/display_client.c, since renamed): still deliberately
   asks for a canvas larger than the server will ever grant (400x300
   vs. the server's 200x150 max), still proves framebuffer-ownership
   exclusivity across processes.

   The ONE new thing this process does: after its own canvas has
   ACTUALLY landed on screen (sys_fb_present has already run inside the
   server, triggered by this process's own DISPLAY_OP_PRESENT), it
   sends display_client_b.c a DISPLAY_OP_GO signal -- the explicit
   hand-off that makes the two windows' resulting z-order deterministic
   BY CONSTRUCTION rather than a race: client B's own
   DISPLAY_OP_REQUEST cannot possibly reach the server before this. */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"

#define REQUESTED_W 400u
#define REQUESTED_H 300u

/* A distinctive teal, packed 0x00RRGGBB (r=0x00, g=0xB4, b=0xC8) --
   far from black (the framebuffer console's own background), the
   mouse cursor's solid red (Milestone 23), and client B's own orange
   (display_client_b.c), so the smoke test's screendump pixel check can
   never mistake any of them for one another. */
#define FILL_COLOR 0x0000B4C8u

static const char msg_ok[] = "[OK] display client A: canvas presented via the display server\n";
static const char msg_bad[] = "[FAIL] display client A: display protocol handshake failed\n";
static const char msg_reject_ok[] = "[OK] display client A: sys_fb_acquire correctly rejected (server already owns the framebuffer)\n";
static const char msg_reject_bug[] = "[FAIL] display client A: sys_fb_acquire incorrectly succeeded despite the server already owning the framebuffer\n";

int main(void)
{
    ipc_message_t boot;
    sys_ipc_recv(&boot); /* kernel_main's own bootstrap message: fields[0] = the display server's pid, fields[1] = client B's pid */
    uint64_t server_pid = boot.fields[0];
    uint64_t client_b_pid = boot.fields[1];

    ipc_message_t req = { .fields = { DISPLAY_OP_REQUEST, REQUESTED_W, REQUESTED_H, 0 } };
    if (sys_ipc_send(server_pid, &req) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    ipc_message_t grant;
    sys_ipc_recv(&grant); /* blocks until the server's DISPLAY_OP_GRANT arrives -- causally AFTER the server's own sys_fb_acquire(), see display_server.c's own comment */
    uint64_t granted_w = grant.fields[3] >> 32;
    uint64_t granted_h = grant.fields[3] & 0xffffffffu;

    /* The cross-process half of the ownership-exclusivity proof: by
       construction (the blocking recv just above), the server has
       ALREADY successfully acquired the framebuffer by the time this
       runs -- no scheduling-order assumption needed. */
    if (sys_fb_acquire() != (uint64_t)-1) {
        sys_write(msg_reject_bug, sizeof(msg_reject_bug) - 1);
        return 1;
    }
    sys_write(msg_reject_ok, sizeof(msg_reject_ok) - 1);

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

    /* Fills EXACTLY the granted canvas -- see display_server.c's own
       Known limitations for why the bound-enforcement proof is
       structural, not just a runtime clamp. */
    uint32_t *buf = (uint32_t *)(uintptr_t)va;
    for (uint64_t i = 0; i < granted_w * granted_h; i++) {
        buf[i] = FILL_COLOR;
    }

    ipc_message_t pres = { .fields = { DISPLAY_OP_PRESENT, shm_id, 0, 0 } };
    if (sys_ipc_send(server_pid, &pres) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    /* sys_ipc_send() only proves DISPLAY_OP_PRESENT was ENQUEUED in the
       server's inbox, never that the server has actually finished (or
       even started) mapping and compositing it -- waiting for the
       server's own DISPLAY_OP_ACK (sent only once sys_fb_present has
       genuinely run, display_server.c) is what turns "this canvas is
       really on screen" into an observable fact this process can act
       on, rather than an assumption. This is the fact the go-signal
       below depends on being true. */
    ipc_message_t ack;
    sys_ipc_recv(&ack);

    sys_write(msg_ok, sizeof(msg_ok) - 1);

    /* Only sent after the ack above -- client B's own
       DISPLAY_OP_REQUEST cannot possibly reach the server before this,
       so client B's window is guaranteed to be presented SECOND,
       making the resulting z-order deterministic by construction, not
       a race. */
    ipc_message_t go = { .fields = { DISPLAY_OP_GO, 0, 0, 0 } };
    sys_ipc_send(client_b_pid, &go);

    return 0;
}
