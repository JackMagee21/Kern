#include <stdint.h>

#include "gdt.h"

#define GDT_ENTRY_COUNT 3

/*
 * Explicit byte-granularity fields rather than C bitfields: bitfield
 * packing order across a byte boundary is implementation-defined, and
 * CLAUDE.md requires GDT/IDT layouts not depend on that (same reasoning
 * kernel/arch/x86_64/boot.asm's hand-written gdt64 documents byte by
 * byte). Field layout: Intel SDM Vol. 3A Sec. 3.4.5 "Segment Descriptors".
 */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity; /* high nibble: flags (G,D/B,L,AVL); low nibble: limit_high */
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

static gdt_entry_t gdt[GDT_ENTRY_COUNT];
static gdt_ptr_t gdt_ptr;

extern void gdt_flush(gdt_ptr_t *ptr);

static void gdt_set_entry(int index, uint8_t access, uint8_t flags)
{
    /* Every descriptor here is flat (base=0, limit=0): limit checks are
       disabled for code/data segments in 64-bit mode, so there is
       nothing for a non-zero limit to protect (Intel SDM Vol. 3A
       Sec. 3.4.5, same fact boot.asm's gdt64 relies on). */
    gdt[index].limit_low  = 0;
    gdt[index].base_low   = 0;
    gdt[index].base_mid   = 0;
    gdt[index].access     = access;
    gdt[index].granularity = (uint8_t)(flags << 4);
    gdt[index].base_high  = 0;
}

void gdt_init(void)
{
    gdt_set_entry(0, 0x00, 0x0);                 /* null descriptor */
    gdt_set_entry(1, 0x9A, 0xA);                  /* kernel code: P DPL0 S exec/read, L=1 D=0 */
    gdt_set_entry(2, 0x92, 0x0);                  /* kernel data: P DPL0 S read/write */

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt[0];

    gdt_flush(&gdt_ptr);
}
