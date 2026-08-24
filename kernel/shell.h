#ifndef KERNEL_SHELL_H
#define KERNEL_SHELL_H

/* Reads a line from the keyboard, echoes it to the console, dispatches
   it to a small set of built-in commands, and repeats forever. Meant
   to run as kernel_main's final act (replacing what used to be a bare
   idle loop) -- it still competes fairly for CPU time with whatever
   else is in the scheduler's round-robin (the Milestone 6/7 demo
   tasks), the same way any other task does; waiting on keyboard input
   via hlt naturally yields the rest of its time slice each round. */
void shell_run(void) __attribute__((noreturn));

#endif /* KERNEL_SHELL_H */
