/* Milestone 28 (ADR 0028): the SECOND of what is now three clients
   (Milestone 33, ADR 0033) kernel/user/display_server.c serves. Waits
   for client A's own DISPLAY_OP_GO (kernel/user/display_client_a.c)
   before ever sending its own DISPLAY_OP_REQUEST -- guaranteeing, by
   construction, that client A's window is fully presented before this
   one's request even reaches the server, so the resulting z-order
   (this window ends up drawn ON TOP of client A's, in the region where
   the server's cascade placement makes them overlap) is deterministic,
   not a race.

   Milestone 33: once THIS window has landed, forwards the exact same
   go-signal on to kernel/user/pulse_app.c -- extending the A -> B -> C
   chain by one more link, the same reasoning as client A's own
   go-signal to this process, just one hop further down. */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"

#define REQUESTED_W 250u
#define REQUESTED_H 200u

/* A distinctive orange, packed 0x00RRGGBB (r=0xFF, g=0x8C, b=0x00) --
   far from black, the cursor's red, and client A's own teal. */
#define FILL_COLOR 0x00FF8C00u

static const char msg_ok[] = "[OK] display client B: canvas presented via the display server\n";
static const char msg_bad[] = "[FAIL] display client B: display protocol handshake failed\n";
static const char msg_reject_ok[] = "[OK] display client B: sys_fb_acquire correctly rejected (server already owns the framebuffer)\n";
static const char msg_reject_bug[] = "[FAIL] display client B: sys_fb_acquire incorrectly succeeded despite the server already owning the framebuffer\n";
static const char msg_go_bad[] = "[FAIL] display client B: go-signal from client A had the wrong opcode\n";

int main(void)
{
    ipc_message_t boot;
    sys_ipc_recv(&boot); /* kernel_main's own bootstrap message: fields[0] = the display server's pid, fields[1] = the pulse app's pid (Milestone 33) */
    uint64_t server_pid = boot.fields[0];
    uint64_t pulse_app_pid = boot.fields[1];

    ipc_message_t go;
    sys_ipc_recv(&go); /* blocks until client A's DISPLAY_OP_GO arrives -- client A's own window is guaranteed already on screen by then, see display_client_a.c */
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

    /* Same cross-process ownership-exclusivity proof client A already
       performs -- still valid here: the server acquired the
       framebuffer long before either client's handshake could reach
       this point. */
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

    uint32_t *buf = (uint32_t *)(uintptr_t)va;
    for (uint64_t i = 0; i < granted_w * granted_h; i++) {
        buf[i] = FILL_COLOR;
    }

    ipc_message_t pres = { .fields = { DISPLAY_OP_PRESENT, shm_id, 0, 0 } };
    if (sys_ipc_send(server_pid, &pres) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    ipc_message_t ack;
    sys_ipc_recv(&ack); /* waits for the server's own confirmation this canvas actually landed on screen -- see display_client_a.c's identical comment */

    sys_write(msg_ok, sizeof(msg_ok) - 1);

    /* Milestone 33: only sent after the ack above -- the pulse app's
       own DISPLAY_OP_REQUEST cannot possibly reach the server before
       this, extending client A's own z-order-determinism guarantee one
       more link down the chain. */
    ipc_message_t go_c = { .fields = { DISPLAY_OP_GO, 0, 0, 0 } };
    sys_ipc_send(pulse_app_pid, &go_c);

    return 0;
}
