#include <stdint.h>

#include "gdt.h"

#define GDT_ENTRY_COUNT 6 /* null, kernel code, kernel data, user32 placeholder, user data, user code */

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

/*
 * 64-bit system descriptor (TSS): the first 6 bytes are laid out
 * identically to gdt_entry_t (same fields, same names) since only a
 * system descriptor needs the extra 8 bytes (base_upper32/reserved) to
 * hold a full 64-bit base -- code/data descriptors ignore base/limit
 * entirely in 64-bit mode, but a system descriptor's base genuinely
 * points somewhere (here, the TSS struct). Verified against Linux's
 * struct ldttss_desc (arch/x86/include/asm/desc_defs.h) field-by-field,
 * translated from its bitfields to explicit bytes for the same reason
 * as gdt_entry_t above.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper32;
    uint32_t reserved;
} tss_descriptor_t;

typedef struct __attribute__((packed)) {
    gdt_entry_t entries[GDT_ENTRY_COUNT];
    tss_descriptor_t tss;
} gdt_table_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

static gdt_table_t gdt_table;
static gdt_ptr_t gdt_ptr;

extern void gdt_flush(gdt_ptr_t *ptr);

static void gdt_set_entry(int index, uint8_t access, uint8_t flags)
{
    /* Every descriptor here is flat (base=0, limit=0): limit checks are
       disabled for code/data segments in 64-bit mode, so there is
       nothing for a non-zero limit to protect (Intel SDM Vol. 3A
       Sec. 3.4.5, same fact boot.asm's gdt64 relies on). */
    gdt_table.entries[index].limit_low   = 0;
    gdt_table.entries[index].base_low    = 0;
    gdt_table.entries[index].base_mid    = 0;
    gdt_table.entries[index].access      = access;
    gdt_table.entries[index].granularity = (uint8_t)(flags << 4);
    gdt_table.entries[index].base_high   = 0;
}

void gdt_set_tss_descriptor(uint64_t base, uint16_t limit)
{
    gdt_table.tss.limit_low    = limit;
    gdt_table.tss.base_low     = (uint16_t)(base & 0xffff);
    gdt_table.tss.base_mid     = (uint8_t)((base >> 16) & 0xff);
    gdt_table.tss.access       = 0x89; /* P=1 DPL=0 S=0(system) Type=0x9 (available 64-bit TSS) */
    gdt_table.tss.granularity  = 0x00; /* G=0 (limit is a byte count, not 4KiB units); limit fits in limit_low */
    gdt_table.tss.base_high    = (uint8_t)((base >> 24) & 0xff);
    gdt_table.tss.base_upper32 = (uint32_t)(base >> 32);
    gdt_table.tss.reserved     = 0;

    __asm__ volatile("ltr %%ax" : : "a"(TSS_SELECTOR));
}

void gdt_init(void)
{
    gdt_set_entry(0, 0x00, 0x0); /* null descriptor */
    gdt_set_entry(1, 0x9A, 0xA); /* kernel code: P DPL0 S exec/read, L=1 D=0 */
    gdt_set_entry(2, 0x92, 0x0); /* kernel data: P DPL0 S read/write */
    gdt_set_entry(3, 0x00, 0x0); /* user32 placeholder: never loaded, see gdt.h */
    gdt_set_entry(4, 0xF2, 0x0); /* user data: P DPL3 S read/write */
    gdt_set_entry(5, 0xFA, 0xA); /* user code: P DPL3 S exec/read, L=1 D=0 */

    gdt_ptr.limit = sizeof(gdt_table) - 1;
    gdt_ptr.base  = (uint64_t)&gdt_table;

    gdt_flush(&gdt_ptr);
}
