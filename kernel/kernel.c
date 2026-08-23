#include <stdint.h>

#include "drivers/serial.h"

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

    for (;;) {
        __asm__ volatile("hlt");
    }
}
