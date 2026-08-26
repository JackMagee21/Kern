#ifndef KERNEL_SHELL_H
#define KERNEL_SHELL_H

#include <stdint.h>

/* Milestone 36 (ADR 0036): lets kernel_main hand the shell the display
   server's pid once it's created, so the shell's own `spawn` command
   can inject a bootstrap message into a dynamically-created client
   the exact same way kernel_main already does for every boot-time
   one. Called exactly once, right after kernel_main creates
   display_server_process -- before that, `spawn` reports "not yet
   available" rather than sending to pid 0 (kernel/user/pulse_app.c's
   own Milestone 36 comment explains why that would be a real bug, not
   just theoretically wrong). */
void shell_set_display_server_pid(uint32_t pid);

/* Reads a line from the keyboard, echoes it to the console, dispatches
   it to a small set of built-in commands, and repeats forever. Meant
   to run as kernel_main's final act (replacing what used to be a bare
   idle loop) -- it still competes fairly for CPU time with whatever
   else is in the scheduler's round-robin (the Milestone 6/7 demo
   tasks), the same way any other task does; waiting on keyboard input
   via hlt naturally yields the rest of its time slice each round. */
void shell_run(void) __attribute__((noreturn));

#endif /* KERNEL_SHELL_H */
