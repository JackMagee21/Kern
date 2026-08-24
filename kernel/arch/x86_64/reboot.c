#include <stdint.h>

#include "reboot.h"
#include "../../../libk/io.h"

/*
 * Legacy 8042 keyboard controller reset. Port, status bit, and command
 * byte verified against the OSDev.org "Reboot" wiki article's
 * documentation of this decades-standard mechanism -- the same class
 * of source this codebase already treats as authoritative for legacy
 * hardware register layouts (ADR 0005, ADR 0013, ADR 0014).
 */
#define KB_CONTROLLER_PORT       0x64u
#define KB_STATUS_INPUT_FULL     (1u << 1)
#define KB_CMD_PULSE_RESET_LINE  0xFEu

/* Same {limit, base} layout as idt.c's own idt_ptr_t (Intel SDM
   Vol. 3A Sec. 6.10's IDTR format) -- kept local rather than shared,
   since the only thing this needs it for is loading a deliberately
   BROKEN one. */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} bad_idt_ptr_t;

void reboot(void)
{
    /* Wait for the controller's input buffer to be empty before
       writing a new command -- writing while it's still processing a
       previous one is undefined per the controller's own spec. */
    while (inb(KB_CONTROLLER_PORT) & KB_STATUS_INPUT_FULL) {
    }
    outb(KB_CONTROLLER_PORT, KB_CMD_PULSE_RESET_LINE);

    /* Fallback: an IDT with limit 0 has no valid entry for ANY vector,
       so the next exception (deliberately triggered here) can't be
       handled, which per Intel SDM Vol. 3A Sec. 6.15 escalates through
       #GP -> #DF -> triple fault -> CPU reset. Unconditional, no
       assumption about the keyboard controller actually being present
       or responsive. */
    bad_idt_ptr_t bad_idt = {0, 0};
    __asm__ volatile("lidt %0" : : "m"(bad_idt));
    __asm__ volatile("int3");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
