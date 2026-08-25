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
   since the KERNEL never interprets these opcodes at all -- they're
   meaningful only to these two specific userspace programs. */

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

#endif /* KERNEL_USER_DISPLAY_PROTOCOL_H */
