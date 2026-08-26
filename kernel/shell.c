#include <stdbool.h>
#include <stdint.h>

#include "shell.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/mouse.h"
#include "drivers/cursor.h"
#include "arch/x86_64/reboot.h"
#include "sched/task.h"
#include "sched/scheduler.h"
#include "ipc/msgqueue.h"
#include "user/display_protocol.h" /* DISPLAY_OP_GO -- see spawn_app()'s own doc comment for why the shell needs this, Milestone 36 (ADR 0036) */
#include "../libk/fmt.h"

/* Milestone 36 (ADR 0036): the SAME embedded images kernel.c already
   declares (kernel/user/embed/pulse_app_blob.asm/clock_app_blob.asm)
   -- `extern` here too so spawn_app() can hand either one to
   task_create_user_image() at runtime, from a real shell command,
   rather than only ever at boot. */
extern const uint8_t pulse_app_image_start[];
extern const uint8_t pulse_app_image_end[];
extern const uint8_t clock_app_image_start[];
extern const uint8_t clock_app_image_end[];

static uint32_t display_server_pid;

void shell_set_display_server_pid(uint32_t pid)
{
    display_server_pid = pid;
}

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
            /* Milestone 23 (ADR 0023): every hlt wakeup here (keyboard,
               mouse, or the 100Hz timer) also gives the mouse cursor a
               chance to redraw at its latest position -- no dedicated
               polling loop or new scheduling primitive needed, since
               this loop already wakes on exactly the interrupts that
               matter. */
            cursor_poll();
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

/* Milestone 36 (ADR 0036): launches a genuinely NEW instance of an
   already-embedded GUI program at runtime -- kernel/user/pulse_app.c
   and kernel/user/clock_app.c are the only two clients that make sense
   to spawn this way (both persistent, both closeable, both requiring
   no other client's own go-signal to get started). A real path-based
   `execve` (launching an ARBITRARY program, not just one of a fixed
   embedded set) remains blocked on the filesystem non-goal
   (CLAUDE.md) -- this is deliberately narrower: the same fixed set of
   programs this kernel has always been able to run, just launched at a
   moment of the user's own choosing instead of only at boot.

   Injects TWO messages, not one -- a boot message (fields[0] = the
   server's pid) exactly like kernel_main's own boot-time injection,
   THEN a synthetic DISPLAY_OP_GO. Both pulse_app.c and clock_app.c
   already unconditionally wait for a go-signal before their own first
   DISPLAY_OP_REQUEST (Milestone 33/35's own go-signal chain) -- rather
   than restructuring either program to make that wait conditional,
   the kernel just satisfies the SAME wait a peer client would have,
   letting a dynamically spawned instance reuse both programs
   completely unmodified. This is the one deliberate, narrow exception
   to display_protocol.h's own "the kernel never interprets these
   opcodes" stance (see that file's own updated doc comment) -- the
   kernel doesn't branch on DISPLAY_OP_GO's meaning here, it just
   constructs the exact message a peer client would have sent. */
static void spawn_app(const uint8_t *image_start, const uint8_t *image_end, const char *label)
{
    if (display_server_pid == 0) {
        console_write("spawn failed: display server not available yet\n");
        return;
    }

    task_t *task = task_create_user_image(image_start, image_end);
    scheduler_add_task(task);

    ipc_message_t boot = { .fields = { display_server_pid, 0, 0, 0 } };
    ipc_message_t go = { .fields = { DISPLAY_OP_GO, 0, 0, 0 } };
    if (!ipc_send(task, &boot) || !ipc_send(task, &go)) {
        console_write("spawn failed: bootstrap inbox somehow already full\n");
        return;
    }

    console_write("[OK] spawned ");
    console_write(label);
    console_write(", pid 0x");
    console_write_hex(task->id);
    console_write("\n");
}

static void run_command(const char *line)
{
    if (line[0] == '\0') {
        return;
    }

    if (str_eq(line, "help")) {
        console_write("commands: help, echo <text>, uptime, date, mouse, reboot, clear, spawn pulse, spawn clock\n");
    } else if (str_eq(line, "spawn pulse")) {
        spawn_app(pulse_app_image_start, pulse_app_image_end, "pulse app");
    } else if (str_eq(line, "spawn clock")) {
        spawn_app(clock_app_image_start, clock_app_image_end, "clock app");
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
