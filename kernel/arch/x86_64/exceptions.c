#include <stdint.h>

#include "idt.h"
#include "trap_frame.h"
#include "../../drivers/console.h"
#include "../../mm/vmm.h"

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
    console_write(label);
    console_write_hex(value);
    console_write("\n");
}

/* CLAUDE.md safety rule 6: on unrecoverable error, print full state to
   serial before halt -- never fail silently, never auto-reboot. There
   is no GENERAL recovery path (no per-process fault isolation for an
   arbitrary fault), so every exception here is still fatal, EXCEPT:
   #BP (vector 3) -- a breakpoint is architecturally meant to be
   resumable (that's the entire point of int3 as a debugging primitive)
   -- and, since Milestone 21 (ADR 0021), a #PF (vector 14) that turns
   out to be a copy-on-write write fault (task_fork(), ADR 0018/0021),
   resolved by vmm_handle_cow_fault() before any of the code below even
   runs. Both are cases this handler returns from normally (iretq
   resumes right at/after the faulting instruction) instead of halting.
   The #BP case is what lets kernel_main's Milestone 2 self-test coexist
   with Milestone 5's requirement that the kernel keep running after
   boot to service timer IRQs. Returns the frame to resume
   (common_stub.inc loads this into RSP before iretq); always the same
   frame it was given -- exceptions never trigger a Milestone 6 task
   switch, only irq_handler's timer path does. */
trap_frame_t *isr_handler(trap_frame_t *frame)
{
    /* Milestone 21 (ADR 0021): a write fault on a copy-on-write page
       (task_fork(), ADR 0018/0021) is EXPECTED, frequent, successful
       control flow -- not a fault to report. Checked and resolved
       here, silently, before any of the diagnostic printing below, the
       same "the common/successful case doesn't get logged" reasoning
       already applied elsewhere in this kernel (e.g. sys_nop). Error
       code bit 0 (P) must be 1 (protection violation on an ALREADY-
       PRESENT page, not a genuine not-present fault) and bit 1 (W/R)
       must be 1 (a write, not a read) -- Intel SDM Vol. 3A Sec. 4.7
       Table 4-12 -- checked before even calling vmm_handle_cow_fault(),
       narrowing what it's ever asked about to exactly the shape a COW
       fault can take; anything else (including a write fault on a page
       that just isn't COW at all) falls through to the same fatal path
       every other exception already takes. */
    if (frame->vector == 14 && (frame->error_code & 0x3) == 0x3) {
        uint64_t fault_addr = read_cr2();
        if (vmm_handle_cow_fault(fault_addr)) {
            return frame; /* resolved -- resume the faulting instruction, which now succeeds */
        }
    }

    console_write("\n[PANIC] exception: ");
    console_write(exception_names[frame->vector]);
    console_write("\n");

    dump_field("  vector:      0x", frame->vector);
    dump_field("  error_code:  0x", frame->error_code);
    if (frame->vector == 14) {
        dump_field("  cr2 (fault): 0x", read_cr2());
        /* Intel SDM Vol. 3A Sec. 4.7 Table 4-12, page-fault error code
           bits: 0=P (0=not-present, 1=protection violation), 1=W/R,
           2=U/S, 3=RSVD (reserved-bit violation -- e.g. VMM_FLAG_NX
           used before vmm_enable_nx() ran), 4=I/D (1=instruction
           fetch, only meaningful once NX is actually enabled -- this
           is what an NX violation looks like: I/D=1, P=1). Decoded
           explicitly rather than left as a raw hex code for the same
           reason every other field here is already spelled out:
           CLAUDE.md safety rule 6, full state, not a puzzle to solve
           after the fact. */
        console_write("  pf reason:   ");
        console_write((frame->error_code & (1u << 0)) ? "protection-violation" : "not-present");
        console_write((frame->error_code & (1u << 1)) ? ", write" : ", read");
        console_write((frame->error_code & (1u << 2)) ? ", user-mode" : ", supervisor-mode");
        if (frame->error_code & (1u << 3)) {
            console_write(", reserved-bit-violation");
        }
        if (frame->error_code & (1u << 4)) {
            console_write(", instruction-fetch");
        }
        console_write("\n");
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

    if (frame->vector == 3) {
        return frame; /* resume exactly where interrupted; exceptions never trigger a task switch */
    }

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
