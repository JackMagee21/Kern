#include "pic.h"
#include "../../libk/io.h"
#include "../arch/x86_64/idt.h" /* IDT_IRQ_VECTOR_BASE */

/*
 * 8259A PIC (master + cascaded slave). Ports, ICW1/ICW4 values, and the
 * non-specific EOI command verified against Linux's own
 * arch/x86/include/asm/i8259.h and arch/x86/kernel/i8259.c
 * (PIC_MASTER_CMD/IMR/PIC_SLAVE_CMD/IMR ports; ICW1=0x11 select-init;
 * ICW4 8086-mode bit). Manual (not auto) EOI is used here, unlike
 * Linux's default -- simpler and more explicit for a kernel that isn't
 * trying to optimize interrupt latency yet.
 */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xa0
#define PIC2_DATA    0xa1

#define PIC_ICW1_INIT 0x11 /* edge-triggered, cascade mode, ICW4 needed */
#define PIC_ICW4_8086 0x01 /* 8086/88 mode, manual EOI */

#define PIC_EOI 0x20 /* OCW2 non-specific EOI command */

#define PIC1_VECTOR_BASE IDT_IRQ_VECTOR_BASE       /* IRQ0-7  -> 32-39 */
#define PIC2_VECTOR_BASE (IDT_IRQ_VECTOR_BASE + 8) /* IRQ8-15 -> 40-47 */

void pic_remap(void)
{
    outb(PIC1_COMMAND, PIC_ICW1_INIT);
    io_wait();
    outb(PIC2_COMMAND, PIC_ICW1_INIT);
    io_wait();

    outb(PIC1_DATA, PIC1_VECTOR_BASE); /* ICW2: master vector offset */
    io_wait();
    outb(PIC2_DATA, PIC2_VECTOR_BASE); /* ICW2: slave vector offset */
    io_wait();

    outb(PIC1_DATA, 0x04); /* ICW3: slave PIC attached at master's IRQ2 (bit 2 set) */
    io_wait();
    outb(PIC2_DATA, 0x02); /* ICW3: slave's own cascade identity (IRQ2) */
    io_wait();

    outb(PIC1_DATA, PIC_ICW4_8086);
    io_wait();
    outb(PIC2_DATA, PIC_ICW4_8086);
    io_wait();

    /* Mask everything: don't inherit whatever state firmware/GRUB left.
       Callers unmask specific lines they actually handle. */
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
}

void pic_send_eoi(uint8_t irq_line)
{
    if (irq_line >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq_line)
{
    uint16_t port = irq_line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq_line < 8 ? irq_line : (uint8_t)(irq_line - 8);
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

void pic_clear_mask(uint8_t irq_line)
{
    uint16_t port = irq_line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq_line < 8 ? irq_line : (uint8_t)(irq_line - 8);
    outb(port, (uint8_t)(inb(port) & ~(1u << bit)));
}
