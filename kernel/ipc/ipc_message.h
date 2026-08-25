#ifndef KERNEL_IPC_IPC_MESSAGE_H
#define KERNEL_IPC_IPC_MESSAGE_H

#include <stdint.h>

/* Milestone 26 (ADR 0026): the wire format for one IPC message --
   defined ONCE here and shared, unmodified, by both kernel-side code
   (kernel/ipc/msgqueue.c) and userspace runtime code (kernel/user/rt/
   syscall.h/.c, via this same header) rather than duplicated on each
   side. A struct LAYOUT mismatch between kernel and user code would
   silently corrupt data (unlike the syscall NUMBERS, which are safely
   duplicated as plain integers on both sides -- a mismatch there would
   at worst misdispatch, not corrupt memory), so this one shared
   definition is load-bearing, not a style preference. Freestanding-safe
   (only <stdint.h>) so a ring-3 program can include it directly.

   Fixed-size, fixed-field-count messages only -- no variable-length
   payload support this milestone (would need bounds-checked
   variable-length copies from user pointers; not needed by anything
   this milestone's own consumers actually send). sender_pid is filled
   in by the KERNEL (kernel/arch/x86_64/syscall.c's sys_ipc_send), never
   trusted from whatever a caller's own message struct happens to
   contain -- the same "don't trust user-supplied identity claims"
   discipline this codebase already applies elsewhere. fields[] is
   generic/untyped on purpose: this milestone's own consumers (Desktop.md's
   shared-memory handoff self-test) only need a handful of small
   integers (an opcode, a shm id, a size) -- a real protocol (window
   open/close, input events) is future work for whichever later
   milestone actually defines one, not designed speculatively here. */
#define IPC_MSG_FIELDS 4

typedef struct {
    uint32_t sender_pid;
    uint64_t fields[IPC_MSG_FIELDS];
} ipc_message_t;

#endif /* KERNEL_IPC_IPC_MESSAGE_H */
