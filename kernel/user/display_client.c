/* Milestone 27 (ADR 0027): the one client of kernel/user/display_server.c
   this milestone builds. Deliberately asks for a canvas LARGER than
   the server's own fixed maximum (400x300 vs. the server's 200x150) --
   proving "the server enforces the bound" (Desktop.md) means something
   real: this process can only ever paint what it was actually granted,
   never what it originally asked for. Learns the server's pid from a
   bootstrap message kernel_main itself injects directly (same reason
   and mechanism as kernel/user/ipc_sender.c, Milestone 26 -- no
   argv/envp yet, ADR 0024's own Known limitations). */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"

#define REQUESTED_W 400u
#define REQUESTED_H 300u

/* A distinctive teal, packed 0x00RRGGBB (r=0x00, g=0xB4, b=0xC8) --
   far from black (the framebuffer console's own background) and from
   the mouse cursor's solid red (kernel/drivers/cursor.c, Milestone 23),
   so the smoke test's screendump pixel check can never mistake one for
   the other. */
#define FILL_COLOR 0x0000B4C8u

static const char msg_ok[] = "[OK] display client: canvas presented via the display server\n";
static const char msg_bad[] = "[FAIL] display client: display protocol handshake failed\n";
static const char msg_reject_ok[] = "[OK] display client: sys_fb_acquire correctly rejected (server already owns the framebuffer)\n";
static const char msg_reject_bug[] = "[FAIL] display client: sys_fb_acquire incorrectly succeeded despite the server already owning the framebuffer\n";

int main(void)
{
    ipc_message_t boot;
    sys_ipc_recv(&boot); /* kernel_main's own bootstrap message: fields[0] = the display server's pid */
    uint64_t server_pid = boot.fields[0];

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

    /* Fills EXACTLY the granted canvas -- this process never even
       allocates a buffer large enough to hold what it originally
       asked for (400x300), so there is no memory anywhere containing
       pixels beyond the granted rectangle for a bug to accidentally
       leak onto the screen; the on-screen proof of "the server
       enforces the bound" is the smoke test's own screendump check
       (tests/qemu/test_display_server_selftest.sh), which confirms
       nothing appears past the granted 200x150 canvas even though this
       client explicitly asked for 400x300. */
    uint32_t *buf = (uint32_t *)(uintptr_t)va;
    for (uint64_t i = 0; i < granted_w * granted_h; i++) {
        buf[i] = FILL_COLOR;
    }

    ipc_message_t pres = { .fields = { DISPLAY_OP_PRESENT, shm_id, 0, 0 } };
    if (sys_ipc_send(server_pid, &pres) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    sys_write(msg_ok, sizeof(msg_ok) - 1);
    return 0;
}
