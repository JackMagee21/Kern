#ifndef KERNEL_IPC_MSGQUEUE_H
#define KERNEL_IPC_MSGQUEUE_H

#include <stdbool.h>

#include "ipc_message.h"
#include "../sched/task.h"

/* Milestone 26 (ADR 0026): pushes msg into dest's own inbox
   (task_t::ipc_inbox, a small fixed-capacity ring buffer). Returns
   false (message dropped) if dest's inbox is currently full -- the
   same "fixed capacity, simplest correct behavior, drop rather than
   block the SENDER" contract kernel/drivers/mouse.c's own event queues
   already establish; a sender blocking on a full inbox would need a
   second wait-queue this milestone doesn't build. Calls
   scheduler_wake(dest) unconditionally on a successful push -- a no-op
   per scheduler_wake()'s own contract if dest isn't currently blocked
   (e.g. it hasn't called sys_ipc_recv yet), so this never needs its own
   "is anyone waiting" check. */
bool ipc_send(task_t *dest, const ipc_message_t *msg);

/* Milestone 26 (ADR 0026): a single, NON-blocking attempt to pop the
   oldest queued message from self's own inbox into *out. Returns false
   (leaving *out untouched) if the inbox is empty. Blocking is layered
   on top of this by the caller (kernel/arch/x86_64/syscall.c's
   sys_ipc_recv, via scheduler_block_current() -- the same
   "non-blocking primitive, blocking built on top via a retry loop"
   shape sys_wait/scheduler_try_wait() already established, kept
   separate on purpose: this function has no business deciding whether
   or how long to wait). */
bool ipc_try_recv(task_t *self, ipc_message_t *out);

#endif /* KERNEL_IPC_MSGQUEUE_H */
