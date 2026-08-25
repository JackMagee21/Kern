#ifndef KERNEL_USER_RT_STRING_H
#define KERNEL_USER_RT_STRING_H

#include <stddef.h>

/* Milestone 24: minimal freestanding subset for userspace C programs --
   both for explicit use (e.g. strlen() before a sys_write()) and
   defensively, since a freestanding C compiler can synthesize implicit
   calls to memcpy/memset/memmove for struct assignments or array
   initializers even in code that never calls them explicitly (the same
   reason kernel-side code needs its own /libk rather than trusting none
   of these ever get emitted). Same "small, single-purpose, byte-loop,
   correctness over cleverness" style as /libk's own functions. */
void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);

#endif /* KERNEL_USER_RT_STRING_H */
