#include <stdint.h>

#include "idt.h"
#include "trap_frame.h"
#include "../drivers/serial.h"

/* Intel SDM Vol. 3A Table 6-1 "Protected-Mode Exceptions and
   Interrupts". Vectors 22-27/31 are architecturally reserved; 28-30 are
   recent/virtualization-specific and won't fire on this kernel. */
static const char *const exception_names[IDT_NUM_EXCEPTION_VECTORS] = {
    [0]  = "#DE Divide Error",
    [1]  = "#DB Debug",
    [2]  = "NMI Interrupt",
    [3]  = "#BP Breakpoint",
    [4]  = "#OF Overflow",
    [5]  = "#BR BOUND Range Exceeded",
    [6]  = "#UD Invalid Opcode",
    [7]  = "#NM Device Not Available",
    [8]  = "#DF Double Fault",
    [9]  = "Coprocessor Segment Overrun (reserved)",
    [10] = "#TS Invalid TSS",
    [11] = "#NP Segment Not Present",
    [12] = "#SS Stack-Segment Fault",
    [13] = "#GP General Protection",
    [14] = "#PF Page Fault",
    [15] = "Reserved",
    [16] = "#MF x87 FPU Floating-Point Error",
    [17] = "#AC Alignment Check",
    [18] = "#MC Machine Check",
    [19] = "#XM SIMD Floating-Point Exception",
    [20] = "#VE Virtualization Exception",
    [21] = "#CP Control Protection Exception",
    [22] = "Reserved", [23] = "Reserved", [24] = "Reserved", [25] = "Reserved",
    [26] = "Reserved", [27] = "Reserved",
    [28] = "#HV Hypervisor Injection Exception",
    [29] = "#VC VMM Communication Exception",
    [30] = "#SX Security Exception",
    [31] = "Reserved",
};

static uint64_t read_cr2(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void dump_field(const char *label, uint64_t value)
{
    serial_write(label);
    serial_write_hex(value);
    serial_write("\n");
}

/* CLAUDE.md safety rule 6: on unrecoverable error, print full state to
   serial before halt -- never fail silently, never auto-reboot. There
   is no recovery path yet (no scheduler, no per-process fault
   isolation), so every exception here is fatal. */
void isr_handler(trap_frame_t *frame)
{
    serial_write("\n[PANIC] exception: ");
    serial_write(exception_names[frame->vector]);
    serial_write("\n");

    dump_field("  vector:      0x", frame->vector);
    dump_field("  error_code:  0x", frame->error_code);
    if (frame->vector == 14) {
        dump_field("  cr2 (fault): 0x", read_cr2());
    }

    dump_field("  rip:         0x", frame->rip);
    dump_field("  cs:          0x", frame->cs);
    dump_field("  rflags:      0x", frame->rflags);
    dump_field("  rsp:         0x", frame->rsp);
    dump_field("  ss:          0x", frame->ss);

    dump_field("  rax:         0x", frame->rax);
    dump_field("  rbx:         0x", frame->rbx);
    dump_field("  rcx:         0x", frame->rcx);
    dump_field("  rdx:         0x", frame->rdx);
    dump_field("  rsi:         0x", frame->rsi);
    dump_field("  rdi:         0x", frame->rdi);
    dump_field("  rbp:         0x", frame->rbp);
    dump_field("  r8:          0x", frame->r8);
    dump_field("  r9:          0x", frame->r9);
    dump_field("  r10:         0x", frame->r10);
    dump_field("  r11:         0x", frame->r11);
    dump_field("  r12:         0x", frame->r12);
    dump_field("  r13:         0x", frame->r13);
    dump_field("  r14:         0x", frame->r14);
    dump_field("  r15:         0x", frame->r15);

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
