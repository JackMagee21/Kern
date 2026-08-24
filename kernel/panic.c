#include "panic.h"
#include "drivers/serial.h"

void panic(const char *message)
{
    serial_write("[PANIC] ");
    serial_write(message);
    serial_write("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
