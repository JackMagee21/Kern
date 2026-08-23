#include <stdint.h>

#include "drivers/serial.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    serial_init();

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        serial_write("[PANIC] invalid multiboot2 magic\n");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    (void)mbi_addr; /* multiboot info parsing lands in a later milestone */

    serial_write("[OK] hello kernel\n");

    gdt_init();
    idt_init();
    serial_write("[OK] gdt/idt installed\n");

    /* Milestone 2 self-test: every exception handler is a terminal fault
       dump for now (no recovery/scheduler exists to resume into), so the
       only way to prove the IDT path works end to end is to deliberately
       take a fault and check its dump. #BP is used because it's benign
       and carries no error code. This line -- and the kernel's ability to
       do anything useful after it -- goes away once Milestone 5/6 give
       handlers somewhere to return to. */
    __asm__ volatile("int3");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
