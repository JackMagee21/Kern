#ifndef KERNEL_ARCH_X86_64_GDT_H
#define KERNEL_ARCH_X86_64_GDT_H

#include <stdint.h>

/*
 * Milestone 7 extends the flat null/kernel-code/kernel-data layout from
 * Milestones 1-2 with user segments and a TSS, in an order that is NOT
 * arbitrary: SYSRET (Intel SDM Vol. 2B / AMD64 APM Vol. 2) reconstructs
 * its target CS/SS from IA32_STAR[63:48] by adding fixed offsets --
 * user SS = STAR[63:48]+8, user CS (64-bit) = STAR[63:48]+16 -- which
 * only works if the GDT has, consecutively: [some base], user data,
 * user code. The slot at the base itself (0x18 here) is the 32-bit
 * compatibility-mode user code segment in the ABI this instruction was
 * designed around; this kernel never runs 32-bit user code, so it's an
 * unused, never-loaded placeholder that exists purely to make the
 * arithmetic land correctly. Verified against Linux's own syscall_init()
 * (arch/x86/kernel/cpu/common.c: STAR = (__USER32_CS << 16) |
 * __KERNEL_CS, with __USER32_CS/__USER_DS/__USER_CS consecutive in its
 * GDT) rather than derived from memory alone -- this exact ordering
 * requirement is a well-known, easy-to-get-wrong SYSCALL/SYSRET gotcha.
 *
 * Similarly, SYSCALL reconstructs kernel CS/SS from STAR[47:32]: kernel
 * SS = STAR[47:32]+8, which is already satisfied by the existing
 * KERNEL_CODE_SELECTOR/KERNEL_DATA_SELECTOR values (0x08, 0x10).
 */
#define KERNEL_CODE_SELECTOR   0x08
#define KERNEL_DATA_SELECTOR   0x10
#define USER32_CS_PLACEHOLDER  0x18 /* never loaded -- exists only for SYSRET's +8/+16 arithmetic */
#define USER_DATA_SELECTOR     0x20
#define USER_CODE_SELECTOR     0x28
#define TSS_SELECTOR           0x30

void gdt_init(void);

/* Fills in the TSS descriptor's base/limit and loads TR via ltr.
   Called by tss_init() (tss.c) once the actual TSS struct exists --
   gdt.c owns the table structure, tss.c owns the TSS content. */
void gdt_set_tss_descriptor(uint64_t base, uint16_t limit);

#endif /* KERNEL_ARCH_X86_64_GDT_H */
