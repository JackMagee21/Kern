#include "msgqueue.h"
#include "../sched/scheduler.h"

bool ipc_send(task_t *dest, const ipc_message_t *msg)
{
    uint32_t next_head = (dest->ipc_inbox_head + 1u) % IPC_INBOX_CAPACITY;
    if (next_head == dest->ipc_inbox_tail) {
        return false; /* inbox full -- drop, matching mouse.c's own event-queue contract */
    }
    dest->ipc_inbox[dest->ipc_inbox_head] = *msg;
    dest->ipc_inbox_head = next_head;

    scheduler_wake(dest); /* no-op if dest isn't currently blocked -- see this file's own header doc comment */
    return true;
}

bool ipc_try_recv(task_t *self, ipc_message_t *out)
{
    if (self->ipc_inbox_head == self->ipc_inbox_tail) {
        return false;
    }
    *out = self->ipc_inbox[self->ipc_inbox_tail];
    self->ipc_inbox_tail = (self->ipc_inbox_tail + 1u) % IPC_INBOX_CAPACITY;
    return true;
}
