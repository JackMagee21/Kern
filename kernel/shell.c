#include <stdbool.h>
#include <stdint.h>

#include "shell.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/mouse.h"
#include "arch/x86_64/reboot.h"
#include "../libk/fmt.h"

#define SHELL_LINE_MAX 128

/* No libk string module yet -- these two tiny, single-purpose helpers
   are all a 4-command shell needs, so they live here rather than
   introducing a new shared module for something this narrow (revisit
   if/when something else also needs string comparison). */
static bool str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static bool str_starts_with(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

/* Zero-padded 2-digit decimal, for date/time fields (u32_to_dec()
   itself never pads -- that's a display concern specific to this
   command, not something the general libk helper should bake in). */
static void write_2digit(uint32_t value)
{
    char digits[11];
    if (value < 10) {
        console_putc('0');
    }
    u32_to_dec(value, digits);
    console_write(digits);
}

static void read_line(char *buf, int max_len)
{
    int len = 0;
    for (;;) {
        while (!keyboard_has_char()) {
            __asm__ volatile("hlt");
        }
        char c = keyboard_getc();

        if (c == '\n') {
            console_putc('\n');
            break;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                console_write("\b \b"); /* back over the char, blank it, back again */
            }
        } else if (len < max_len - 1 && c >= 32 && c < 127) {
            buf[len++] = c;
            console_putc(c);
        }
        /* other control characters (tab, esc, ...) are silently ignored -- not needed yet */
    }
    buf[len] = '\0';
}

static void run_command(const char *line)
{
    if (line[0] == '\0') {
        return;
    }

    if (str_eq(line, "help")) {
        console_write("commands: help, echo <text>, uptime, date, mouse, reboot, clear\n");
    } else if (str_eq(line, "mouse")) {
        console_write("waiting for a mouse event (move it or click)...\n");
        while (!mouse_has_event()) {
            __asm__ volatile("hlt");
        }
        mouse_event_t event = mouse_get_event();
        char digits[11];

        console_write("dx=");
        if (event.dx < 0) {
            console_putc('-');
        }
        u32_to_dec((uint32_t)(event.dx < 0 ? -event.dx : event.dx), digits);
        console_write(digits);

        console_write(" dy=");
        if (event.dy < 0) {
            console_putc('-');
        }
        u32_to_dec((uint32_t)(event.dy < 0 ? -event.dy : event.dy), digits);
        console_write(digits);

        console_write(" buttons: L=");
        console_putc(event.left ? '1' : '0');
        console_write(" R=");
        console_putc(event.right ? '1' : '0');
        console_write(" M=");
        console_putc(event.middle ? '1' : '0');
        console_write("\n");
    } else if (str_eq(line, "reboot")) {
        console_write("rebooting...\n");
        reboot(); /* noreturn */
    } else if (str_eq(line, "uptime")) {
        console_write("ticks: 0x");
        console_write_hex(pit_get_ticks());
        console_write("\n");
    } else if (str_eq(line, "date")) {
        rtc_time_t now;
        rtc_read(&now);

        char year_digits[11];
        u32_to_dec(now.year, year_digits);
        console_write(year_digits);
        console_putc('-');
        write_2digit(now.month);
        console_putc('-');
        write_2digit(now.day);
        console_putc(' ');
        write_2digit(now.hour);
        console_putc(':');
        write_2digit(now.minute);
        console_putc(':');
        write_2digit(now.second);
        console_write(" UTC (from CMOS RTC)\n");
    } else if (str_eq(line, "clear")) {
        console_clear();
    } else if (str_eq(line, "echo")) {
        console_write("\n");
    } else if (str_starts_with(line, "echo ")) {
        console_write(line + 5);
        console_write("\n");
    } else {
        console_write("unknown command: ");
        console_write(line);
        console_write("\n");
    }
}

void shell_run(void)
{
    char line[SHELL_LINE_MAX];

    console_write("\nkernel shell -- type 'help' for commands\n");
    for (;;) {
        console_write("> ");
        read_line(line, SHELL_LINE_MAX);
        run_command(line);
    }
}
