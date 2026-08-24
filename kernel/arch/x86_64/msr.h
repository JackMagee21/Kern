#ifndef KERNEL_ARCH_X86_64_MSR_H
#define KERNEL_ARCH_X86_64_MSR_H

#include <stdint.h>

/* Plain RDMSR/WRMSR wrappers -- Intel SDM Vol. 2B, same encoding every
   MSR access in this kernel already relies on (originally written once
   in syscall.c for STAR/LSTAR/SFMASK/EFER.SCE; factored out here once
   vmm.c also needed EFER.NXE, rather than duplicating the exact same
   six lines twice). static inline, not extern: trivial enough that a
   real call/return would be pure overhead, and this is a genuinely
   freestanding-safe use of `static inline` in a header (no ODR issues
   -- each translation unit gets its own copy, nothing shared). */
static inline void write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xffffffffu);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

#endif /* KERNEL_ARCH_X86_64_MSR_H */
