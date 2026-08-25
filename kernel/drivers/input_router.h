#ifndef KERNEL_DRIVERS_INPUT_ROUTER_H
#define KERNEL_DRIVERS_INPUT_ROUTER_H

#include <stdint.h>

/* Milestone 29 (ADR 0029): the bridge between a real hardware input
   event and a ring-3 process -- the first thing in this kernel that
   turns an IRQ-driven device event into IPC delivery
   (kernel/ipc/msgqueue.c, Milestone 26), rather than something only
   kernel-side code (cursor_poll(), the shell's `mouse` command)
   consumes. Deliberately narrow: exactly one event kind (a left-click
   edge), delivered to exactly one subscriber
   (syscall_get_input_focus_pid(), kernel/arch/x86_64/syscall.c) -- not
   a general input-event bus, since nothing this milestone needs one
   yet (CLAUDE.md: don't build for a hypothetical future requirement).
   kernel/drivers/cursor.c is this module's only caller: it already
   sees every mouse event's button state (mouse.h's mouse_event_t) and
   already tracks the authoritative on-screen cursor position, so it's
   the natural place to detect the actual click EDGE (a transition,
   not a level) before calling in here. */

/* Delivers an INPUT_EVENT_CLICK (kernel/user/input_protocol.h) to the
   currently subscribed process, if any -- a silent no-op if nobody has
   ever called sys_input_subscribe() yet, or if the subscriber has
   already exited (scheduler_find_task() returns NULL), the same
   "lossy is fine, nothing here needs guaranteed delivery" stance
   kernel/drivers/mouse.c's own fixed-capacity event queues already
   take. x/y are screen pixel coordinates. */
void input_router_notify_click(uint32_t x, uint32_t y);

/* Milestone 29 (ADR 0029): total number of clicks actually routed to a
   live subscriber so far -- lets kernel_main's self-test prove this
   new delivery path was genuinely exercised, not just that the syscall
   dispatches without crashing, the same "prove the new behavior was
   actually exercised" pattern syscall_get_exec_count()/
   syscall_get_fb_present_count() already established. Does NOT count a
   click that arrived with no subscriber, or with a subscriber that had
   already exited -- only a REAL, delivered routing counts. */
uint64_t input_router_get_click_count(void);

#endif /* KERNEL_DRIVERS_INPUT_ROUTER_H */
