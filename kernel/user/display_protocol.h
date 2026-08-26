#ifndef KERNEL_USER_DISPLAY_PROTOCOL_H
#define KERNEL_USER_DISPLAY_PROTOCOL_H

/* Milestone 27 (ADR 0027): the tiny message protocol
   kernel/user/display_server.c and kernel/user/display_client.c speak
   over the EXISTING ipc_message_t/sys_ipc_send/sys_ipc_recv mechanism
   (Milestone 26) -- fields[0] is always the opcode below, the rest is
   opcode-specific. Deliberately minimal (three message shapes, no
   general "protocol version"/extensibility machinery) since this
   milestone has exactly one client and one server and no multi-window
   logic yet (Desktop.md); a real protocol for window open/close/input
   events is future work for whichever later milestone actually needs
   one, same "don't design for a hypothetical future requirement"
   stance kernel/ipc/ipc_message.h's own doc comment already takes.
   Lives under kernel/user/ (not kernel/ipc/, unlike ipc_message.h)
   since the KERNEL never interprets (branches on) these opcodes --
   they're meaningful only to userspace programs that speak this
   protocol. Milestone 36 (ADR 0036) added one narrow exception:
   kernel/shell.c's `spawn` command CONSTRUCTS a DISPLAY_OP_GO message
   (see that opcode's own doc comment) to hand a dynamically-launched
   client the exact go-signal a peer client would otherwise have sent
   it -- the kernel still never branches on what DISPLAY_OP_GO MEANS,
   it just reuses the wire value so the spawned program's own
   Milestone 33/35 go-signal wait needs no special-casing for how it
   was started. */

/* Client -> server. fields[1] = requested canvas width (pixels),
   fields[2] = requested canvas height (pixels), fields[3] unused. The
   server is never obligated to grant exactly this -- see
   DISPLAY_OP_GRANT. */
#define DISPLAY_OP_REQUEST 1

/* Server -> client, in reply to a DISPLAY_OP_REQUEST. fields[1] = the
   on-screen x of the granted canvas, fields[2] = the on-screen y,
   fields[3] = (granted_width << 32) | granted_height -- packed into
   one field since ipc_message_t only carries 4, the same bit-packing
   this codebase already uses for sys_fb_acquire()'s own return value.
   The granted size may be smaller than what was requested -- THIS is
   "the server enforces the bound" (Desktop.md): kernel/user/
   display_server.c never grants more than its own fixed maximum
   canvas, regardless of what a client asks for. */
#define DISPLAY_OP_GRANT 2

/* Client -> server. fields[1] = a shared-memory object id (from
   sys_shm_create(), already sized to EXACTLY the granted width *
   height * 4 bytes and filled with the client's own pixel data,
   0x00RRGGBB per pixel, row-major, tightly packed) the client wants
   the server to composite onto the real framebuffer. fields[2]/[3]
   unused. */
#define DISPLAY_OP_PRESENT 3

/* Milestone 28 (ADR 0028): client -> client (NOT server -- this is the
   one message in this protocol two CLIENTS exchange directly with each
   other over the same sys_ipc_send()/sys_ipc_recv() mechanism, proving
   nothing about IPC restricts it to client<->server traffic). No
   fields used. kernel/user/display_client_a.c sends this to
   kernel/user/display_client_b.c only after its OWN DISPLAY_OP_PRESENT
   has actually landed -- the deliberate hand-off that makes this
   milestone's two-window z-order demo deterministic BY CONSTRUCTION
   (client B's own DISPLAY_OP_REQUEST cannot reach the server before
   client A's canvas is already on screen), the same "explicit go/
   no-go handoff, not a timing assumption" discipline Milestone 20's
   own ADR already established as strictly more robust. */
#define DISPLAY_OP_GO 4

/* Milestone 28 (ADR 0028): server -> client, sent only AFTER the
   server has actually finished mapping and compositing (sys_fb_present)
   that specific client's DISPLAY_OP_PRESENT -- the missing link that
   makes "client A's canvas is really on screen" an observable,
   waitable fact rather than an assumption. sys_ipc_send() only ever
   proves a message was ENQUEUED in the destination's inbox, never that
   the destination has finished (or even started) acting on it -- a
   client that wants to know its canvas actually landed (or, for client
   A specifically, that must not signal client B to start until its
   OWN canvas is already visible) has to wait for this. No fields
   used. */
#define DISPLAY_OP_ACK 5

/* Milestone 33 (ADR 0033): client -> server, sent any number of times
   AFTER a client's own initial DISPLAY_OP_REQUEST/PRESENT/ACK
   handshake has already completed -- the first message this protocol
   has ever needed for a client that keeps running (every earlier
   client presented exactly once, then exited). No fields used: the
   client has ALREADY written fresh pixel data into the SAME
   shared-memory buffer it originally handed over (still mapped at the
   same address, sys_shm_map() was never repeated -- a client re-maps
   nothing, it just keeps writing into what it already owns), so this
   message means only "re-composite everything -- my content changed."
   The server doesn't need to know WHICH window sent this or even look
   it up: since every window's own stored `va` already points at
   whatever that client most recently wrote, kernel/user/
   display_server.c's own composite_all() naturally picks up the fresh
   content for free, the same "opaque windows, painted bottom-to-top"
   reasoning Milestone 28 already established -- no new per-window
   bookkeeping needed at all. */
#define DISPLAY_OP_REDRAW 6

/* Milestone 34 (ADR 0034): server -> client, sent when this specific
   client's window is closed (its close button was clicked) AND the
   server still remembers which pid granted that window (window_t's own
   `pid` field, kernel/user/display_server.c, filled in once at grant
   time from the DISPLAY_OP_REQUEST's sender_pid). No fields used: the
   only meaningful content is "you, specifically, should exit now."
   Harmless to send to a client that has ALREADY exited on its own
   (clients A/B, display_client_a.c/_b.c, present once then return) --
   sys_ipc_send() to a pid the scheduler no longer recognizes just fails
   (returns -1), silently, the same as any other message to a pid that
   no longer exists; pids are never recycled (scheduler.c), so this can
   never be misdelivered to an unrelated LATER process. A client that
   actually still needs to notice this (kernel/user/pulse_app.c, the
   only client so far that runs forever) polls for it with the
   non-blocking sys_ipc_try_recv() rather than giving up its own
   animation pacing to block waiting for one that might never come. */
#define DISPLAY_OP_EXIT 7

#endif /* KERNEL_USER_DISPLAY_PROTOCOL_H */
