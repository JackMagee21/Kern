#include "panic.h"
#include "drivers/console.h"

void panic(const char *message)
{
    console_write("[PANIC] ");
    console_write(message);
    console_write("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
