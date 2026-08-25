#ifndef KERNEL_USER_RT_SYSCALL_H
#define KERNEL_USER_RT_SYSCALL_H

#include <stdint.h>

#include "../../ipc/ipc_message.h" /* ipc_message_t -- the shared kernel/user wire format, Milestone 26 */

/* Milestone 24: thin wrappers around this kernel's own syscall ABI
   (kernel/arch/x86_64/syscall.h's SYS_* numbers and calling convention
   -- rdi/rsi for the first two args, rax for the number in/return
   value out; every syscall this kernel has ever needed so far fits in
   two args) for C programs built with the minimal userspace runtime
   (kernel/user/rt/). NOT a POSIX syscall wrapper library -- these
   names/signatures match THIS kernel's own small, custom syscall set
   exactly, nothing more (see Desktop.md for why this distinction is
   deliberate, not incidental). */

uint64_t sys_write(const void *buf, uint64_t len);
_Noreturn void sys_exit(uint64_t code);
uint64_t sys_fork(void);
uint64_t sys_wait(uint64_t target_pid, uint64_t *out_exit_code);
uint64_t sys_exec(uint64_t program_id);
void sys_nop(void);

/* Milestone 26 (ADR 0026): returns 0 on SUCCESS (matching this
   kernel's syscall.c dispatcher, which has no other meaningful value
   to report -- NOT the same convention as sys_shm_create()/
   sys_shm_map() below, where 0 means failure because those DO have a
   real id/address to report on success). Returns (uint64_t)-1 on
   failure (destination pid not found, or its inbox is full) -- the
   same failure sentinel sys_write/sys_wait/sys_exec already use, so
   callers that care should check "== (uint64_t)-1", not "== 0". */
uint64_t sys_ipc_send(uint64_t dest_pid, const ipc_message_t *msg);
/* Always blocks until a message arrives; writes it into *out. */
void sys_ipc_recv(ipc_message_t *out);
/* Returns a new shared-memory object id (never 0), or 0 on failure. */
uint64_t sys_shm_create(uint64_t size);
/* Maps shm_id into the caller's OWN address space; returns the mapped
   virtual address (never 0), or 0 on failure. */
uint64_t sys_shm_map(uint64_t shm_id);

#endif /* KERNEL_USER_RT_SYSCALL_H */
