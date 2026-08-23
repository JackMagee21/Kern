#include <stdint.h>

#include "gdt.h"
#include "idt.h"

/* 64-bit IDT gate descriptor, Intel SDM Vol. 3A Sec. 6.14.1
   "64-Bit Mode IDT" / Figure 6-8. Explicit byte fields, same rationale
   as gdt.c: no C bitfields for a hardware-defined layout. */
typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;           /* bits 0-2: IST index (0 = don't switch stacks); rest reserved=0 */
    uint8_t  type_attr;     /* P(1) DPL(2) 0(1) type(4) */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_gate_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

static idt_gate_t idt[256];
static idt_ptr_t idt_ptr;

/* One stub per exception vector, defined in isr.asm. Declared
   individually (not looped/computed) so a typo'd vector number is a
   link error, not a silent off-by-one into the wrong handler. */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

static void (*const isr_stub[IDT_NUM_EXCEPTION_VECTORS])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static void idt_set_gate(int vector, void (*handler)(void), uint16_t selector, uint8_t ist, uint8_t type_attr)
{
    uint64_t addr = (uint64_t)handler;

    idt[vector].offset_low  = (uint16_t)(addr & 0xffff);
    idt[vector].selector    = selector;
    idt[vector].ist         = ist & 0x7;
    idt[vector].type_attr   = type_attr;
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xffff);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xffffffff);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    for (int vector = 0; vector < 256; vector++) {
        idt[vector] = (idt_gate_t){0}; /* not-present: unhandled vectors fault into #GP */
    }

    uint8_t present_dpl0_interrupt_gate = (uint8_t)(0x80 | IDT_GATE_TYPE_INTERRUPT_64);

    for (int vector = 0; vector < IDT_NUM_EXCEPTION_VECTORS; vector++) {
        idt_set_gate(vector, isr_stub[vector], KERNEL_CODE_SELECTOR, 0, present_dpl0_interrupt_gate);
    }

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt[0];

    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}
