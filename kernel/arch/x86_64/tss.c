#include <stdint.h>

#include "tss.h"
#include "gdt.h"
#include "../../mm/heap.h"
#include "../../panic.h"

/*
 * 64-bit TSS layout verified against Linux's struct x86_hw_tss
 * (arch/x86/include/asm/processor.h), not derived from memory alone --
 * exact byte offsets matter here since the CPU reads this structure
 * directly on any ring-3-to-ring-0 transition. Long mode's TSS no
 * longer holds general-purpose register state (unlike the legacy 32-bit
 * TSS) -- just the privilege-level stack pointers (rsp0-2, only rsp0 is
 * used here, since we never run at ring 1/2) and the IST stack table
 * (unused so far -- ADR 0002 deferred it until it's load-bearing;
 * still not needed by this milestone).
 */
typedef struct __attribute__((packed)) {
    uint32_t reserved1;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved2;
    uint64_t ist[7];
    uint32_t reserved3;
    uint32_t reserved4;
    uint16_t reserved5;
    uint16_t io_bitmap_base;
} tss_t;

#define TSS_DEFAULT_STACK_SIZE (16u * 1024u)

static tss_t tss;

void tss_init(void)
{
    uint8_t *stack = (uint8_t *)kmalloc(TSS_DEFAULT_STACK_SIZE);
    if (stack == NULL) {
        panic("tss_init: kmalloc failed for the default RSP0 stack");
    }

    tss.reserved1 = 0;
    tss.rsp0 = (uint64_t)(stack + TSS_DEFAULT_STACK_SIZE);
    tss.rsp1 = 0;
    tss.rsp2 = 0;
    tss.reserved2 = 0;
    for (int i = 0; i < 7; i++) {
        tss.ist[i] = 0;
    }
    tss.reserved3 = 0;
    tss.reserved4 = 0;
    tss.reserved5 = 0;
    tss.io_bitmap_base = sizeof(tss_t); /* no I/O bitmap: base >= TSS limit means "none" */

    gdt_set_tss_descriptor((uint64_t)&tss, sizeof(tss_t) - 1);
}

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}
