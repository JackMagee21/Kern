#include "console.h"
#include "serial.h"
#include "fbconsole.h"
#include "../../libk/fmt.h"

void console_putc(char c)
{
    serial_putc(c);
    fbconsole_putc(c);
}

void console_write(const char *s)
{
    while (*s != '\0') {
        console_putc(*s);
        s++;
    }
}

void console_write_hex(uint64_t value)
{
    char buf[17];
    u64_to_hex(value, buf);
    console_write(buf);
}

void console_clear(void)
{
    serial_write("\x1b[2J\x1b[H");
    fbconsole_clear();
}
