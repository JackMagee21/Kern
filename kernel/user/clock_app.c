/* Milestone 35 (ADR 0035): a real clock -- Desktop.md's own GUI arc is
   already complete (Milestone 33), and Milestone 33's own Rejected
   alternatives explicitly deferred this exact idea ("a real
   wall-clock-driven animation... no sys_* syscall wrapper exists for
   [rtc_read()] yet... a real clock app is reasonable, clearly-scoped
   future work once one does") -- Milestone 34 has since added the
   small piece this needed (sys_ipc_try_recv, letting a persistent
   client notice a close without blocking its own loop), and this
   milestone adds the other missing piece (sys_rtc_read, a syscall
   wrapper around kernel/drivers/rtc.c's existing kernel-only
   rtc_read()) to finally build it.

   A fourth window, alongside the pulse app (Milestone 33) as the third
   -- joins the SAME go-signal chain one link further (A -> B -> pulse
   app -> this), the same "the server's fixed per-slot window_x/
   window_y assignment is deterministic BY CONSTRUCTION" reasoning
   every earlier link in this chain already established.

   Renders real HH:MM:SS using a tiny embedded 3x5-pixel digit font
   (digits 0-9 plus a colon), NOT a dependency on kernel/drivers/
   fbconsole.c's own font/console machinery -- that font is tied to
   the kernel's own text console (rows/cols, not an arbitrary buffer a
   userspace client owns), and Milestone 33's own Rejected alternatives
   already reasoned through why pulling in real text rendering as a
   new subsystem wasn't warranted THEN. It still isn't: eleven small
   hardcoded glyphs (not a general font) is a self-contained, bounded
   addition, not a new subsystem -- no kernel involvement, no shared
   state, just pixels this client already owns how to write.

   Only redraws (and only pings the server) when the displayed SECOND
   actually changes -- polling sys_rtc_read() far more often than that
   would just waste cycles re-drawing pixels that are already correct
   on screen. */

#include <stdint.h>

#include "rt/syscall.h"
#include "display_protocol.h"

#define REQUESTED_W 190u
#define REQUESTED_H 50u

/* Milestone 35: how many sys_nop iterations pass between polls of the
   real clock -- deliberately smaller than pulse_app.c's own
   FRAME_DELAY_NOPS (200000): that app only needs to feel "alive", this
   one should feel like it updates close to every real second. Still
   only actually redraws (see main()'s own last_second check) when the
   second has genuinely changed, so a smaller poll interval here costs
   a cheap rtc_read() and comparison, not extra redraws. */
#define FRAME_DELAY_NOPS 50000u

#define GLYPH_W 3u
#define GLYPH_H 5u
#define GLYPH_SCALE 6u
#define GLYPH_ADVANCE ((GLYPH_W + 1u) * GLYPH_SCALE) /* one glyph plus one column of spacing, both scaled */
#define GLYPH_COUNT 8u /* "HH:MM:SS" */
#define COLON_INDEX 10u

/* Each row is a 3-bit pattern, MSB = leftmost column. A plain blocky
   seven-segment-style digit set -- legible at GLYPH_SCALE, nothing
   fancier needed for eleven fixed glyphs. */
static const uint8_t digit_glyphs[11][GLYPH_H] = {
    { 0x7u, 0x5u, 0x5u, 0x5u, 0x7u }, /* 0 */
    { 0x2u, 0x2u, 0x2u, 0x2u, 0x2u }, /* 1 */
    { 0x7u, 0x1u, 0x7u, 0x4u, 0x7u }, /* 2 */
    { 0x7u, 0x1u, 0x7u, 0x1u, 0x7u }, /* 3 */
    { 0x5u, 0x5u, 0x7u, 0x1u, 0x1u }, /* 4 */
    { 0x7u, 0x4u, 0x7u, 0x1u, 0x7u }, /* 5 */
    { 0x7u, 0x4u, 0x7u, 0x5u, 0x7u }, /* 6 */
    { 0x7u, 0x1u, 0x1u, 0x1u, 0x1u }, /* 7 */
    { 0x7u, 0x5u, 0x7u, 0x5u, 0x7u }, /* 8 */
    { 0x7u, 0x5u, 0x7u, 0x1u, 0x7u }, /* 9 */
    { 0x0u, 0x2u, 0x0u, 0x2u, 0x0u }, /* : */
};

/* A dark navy background and a bright cyan digit color -- both checked
   directly (not guessed) against every color matcher in every existing
   tests/qemu smoke test's own Python assertions before picking these,
   the same discipline ADR 0033's own palette fix established after
   getting this wrong once already. Neither r<20,165<=g<=195,185<=b<=215
   (teal) nor r>235,125<=g<=155,b<20 (orange) nor r>200,g<60,b<60
   (cursor red) nor any of the pulse app's own four palette matchers
   can match either of these. */
#define BG_COLOR    0x00101030u
#define DIGIT_COLOR 0x0000FFFFu

static const char msg_ok[] = "[OK] clock app: canvas presented via the display server, now ticking\n";
static const char msg_bad[] = "[FAIL] clock app: display protocol handshake failed\n";
static const char msg_go_bad[] = "[FAIL] clock app: go-signal from the pulse app had the wrong opcode\n";
static const char msg_exit[] = "[OK] clock app: received exit request, exiting\n";

/* Fills `buf` (granted_w * granted_h pixels) with the background, then
   draws "HH:MM:SS" (real, decimal, zero-padded) centered in it, using
   only integer arithmetic -- no floating point anywhere in this file,
   matching CLAUDE.md's "no FP/SSE until FPU context-switch milestone
   exists" (still true; this milestone doesn't touch that). */
static void render_time(uint32_t *buf, uint64_t w, uint64_t h, const rtc_time_t *t)
{
    for (uint64_t i = 0; i < w * h; i++) {
        buf[i] = BG_COLOR;
    }

    uint32_t digits[GLYPH_COUNT] = {
        (uint32_t)(t->hour / 10u),   (uint32_t)(t->hour % 10u),   COLON_INDEX,
        (uint32_t)(t->minute / 10u), (uint32_t)(t->minute % 10u), COLON_INDEX,
        (uint32_t)(t->second / 10u), (uint32_t)(t->second % 10u),
    };

    uint64_t text_w = GLYPH_COUNT * GLYPH_ADVANCE - GLYPH_SCALE; /* no trailing spacing after the last glyph */
    uint64_t text_h = GLYPH_H * GLYPH_SCALE;
    uint64_t start_x = (w > text_w) ? (w - text_w) / 2u : 0u;
    uint64_t start_y = (h > text_h) ? (h - text_h) / 2u : 0u;

    for (uint32_t ch = 0; ch < GLYPH_COUNT; ch++) {
        const uint8_t *glyph = digit_glyphs[digits[ch]];
        uint64_t glyph_x0 = start_x + ch * GLYPH_ADVANCE;

        for (uint32_t row = 0; row < GLYPH_H; row++) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < GLYPH_W; col++) {
                if (!(bits & (1u << (GLYPH_W - 1u - col)))) {
                    continue;
                }
                uint64_t block_x0 = glyph_x0 + col * GLYPH_SCALE;
                uint64_t block_y0 = start_y + row * GLYPH_SCALE;
                for (uint32_t sy = 0; sy < GLYPH_SCALE; sy++) {
                    for (uint32_t sx = 0; sx < GLYPH_SCALE; sx++) {
                        uint64_t px = block_x0 + sx;
                        uint64_t py = block_y0 + sy;
                        if (px < w && py < h) {
                            buf[py * w + px] = DIGIT_COLOR;
                        }
                    }
                }
            }
        }
    }
}

int main(void)
{
    ipc_message_t boot;
    sys_ipc_recv(&boot); /* kernel_main's own bootstrap message: fields[0] = the display server's pid */
    uint64_t server_pid = boot.fields[0];

    ipc_message_t go;
    sys_ipc_recv(&go); /* blocks until the pulse app's own DISPLAY_OP_GO arrives -- A, B, and the pulse app's windows are all guaranteed already on screen by then */
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
    rtc_time_t last_time;
    sys_rtc_read(&last_time);
    render_time(buf, granted_w, granted_h, &last_time);

    ipc_message_t pres = { .fields = { DISPLAY_OP_PRESENT, shm_id, 0, 0 } };
    if (sys_ipc_send(server_pid, &pres) == (uint64_t)-1) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
        return 1;
    }

    ipc_message_t ack;
    sys_ipc_recv(&ack); /* waits for the server's own confirmation this canvas actually landed -- see display_client_a.c's identical comment */

    sys_write(msg_ok, sizeof(msg_ok) - 1);

    /* Milestone 35: like the pulse app (Milestone 33), this process
       never exits on its own -- it keeps rewriting its OWN
       already-mapped canvas and pinging the server with
       DISPLAY_OP_REDRAW. Unlike the pulse app, it only actually does
       either when the real second has genuinely changed (see
       last_time.second's own check below), not on every poll --
       there's no reason to recomposite pixels that are already
       correct. Supports a real close from the moment it's created
       (Milestone 34's DISPLAY_OP_EXIT, polled non-blockingly with
       sys_ipc_try_recv), not retrofitted later. */
    for (;;) {
        for (uint64_t spin = 0; spin < FRAME_DELAY_NOPS; spin++) {
            sys_nop();
        }

        ipc_message_t msg;
        if (sys_ipc_try_recv(&msg) == 0 && msg.fields[0] == DISPLAY_OP_EXIT) {
            sys_write(msg_exit, sizeof(msg_exit) - 1);
            sys_exit(0);
        }

        rtc_time_t now;
        sys_rtc_read(&now);
        if (now.second != last_time.second) {
            last_time = now;
            render_time(buf, granted_w, granted_h, &now);
            ipc_message_t redraw = { .fields = { DISPLAY_OP_REDRAW, 0, 0, 0 } };
            sys_ipc_send(server_pid, &redraw);
        }
    }
}
