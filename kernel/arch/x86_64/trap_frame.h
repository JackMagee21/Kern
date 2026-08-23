#ifndef KERNEL_ARCH_X86_64_TRAP_FRAME_H
#define KERNEL_ARCH_X86_64_TRAP_FRAME_H

#include <stdint.h>

/*
 * Field order here is a contract with kernel/arch/x86_64/isr.asm's
 * isr_common_stub, NOT an external ABI -- it must exactly match the
 * order isr_common_stub pushes registers in (increasing struct offset =
 * increasing stack address = earlier push), otherwise register values
 * silently swap with no compile error (CLAUDE.md's trap-frame gotcha).
 *
 * rip/cs/rflags/rsp/ss are pushed by the CPU itself on exception entry.
 * In 64-bit mode this SS:RSP pair is always pushed, even without a
 * privilege-level change (unlike legacy 32-bit protected mode) -- this
 * is documented AMD64/Intel long-mode interrupt-frame behavior, and
 * isr.asm's alignment handling assumes it.
 *
 * Confirmed by observation, not just documentation: the kernel_main
 * int3 self-test's fault dump in a real QEMU run showed cs=0x8 and
 * ss=0x10 (exactly KERNEL_CODE_SELECTOR/KERNEL_DATA_SELECTOR from
 * gdt.h) and a sane rip/rsp/rbp -- if SS:RSP weren't actually pushed
 * here, this struct's ss/rsp fields would be reading whatever
 * push_registers left on the stack instead, which would not by
 * coincidence equal the real data selector.
 */
typedef struct __attribute__((packed)) trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} trap_frame_t;

/* Defined in exceptions.c, called from kernel/arch/x86_64/isr.asm's
   isr_common_stub. */
void isr_handler(trap_frame_t *frame);

#endif /* KERNEL_ARCH_X86_64_TRAP_FRAME_H */
