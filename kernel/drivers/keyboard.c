#include <stdint.h>

#include "keyboard.h"
#include "../../libk/io.h"
#include "../../libk/ring_buffer.h"
#include "../arch/x86_64/irq.h"

/*
 * PS/2 keyboard, IRQ1, data port 0x60. Scancode Set 1 (the power-on
 * default every PS/2-compatible keyboard controller emits), US QWERTY
 * layout. Standard, long-stable hardware/layout facts, not a bit-exact
 * structure whose error mode is a silent triple fault -- a wrong table
 * entry just makes one key produce the wrong character, so this wasn't
 * held to the same primary-source-fetch bar as GDT/IDT/page-table
 * layouts (same reasoning as vga.c's CRTC ports).
 *
 * Only handles make codes (key press) for the keys listed; unlisted
 * scancodes (function keys, arrows, numpad, etc.) are silently ignored
 * -- not needed for a line-oriented shell yet.
 */
#define KB_DATA_PORT 0x60

#define SCANCODE_LEFT_SHIFT  0x2a
#define SCANCODE_RIGHT_SHIFT 0x36
#define SCANCODE_RELEASE_BIT 0x80
#define SCANCODE_TABLE_SIZE  0x3a /* covers 0x00-0x39 */

static const char unshifted_table[SCANCODE_TABLE_SIZE] = {
    /* 0x00 */ 0,
    /* 0x01 */ 27, /* Esc */
    /* 0x02 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    /* 0x0e */ '\b',
    /* 0x0f */ '\t',
    /* 0x10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    /* 0x1c */ '\n',
    /* 0x1d */ 0, /* Left Ctrl */
    /* 0x1e */ 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    /* 0x2a */ 0, /* Left Shift */
    /* 0x2b */ '\\',
    /* 0x2c */ 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    /* 0x36 */ 0, /* Right Shift */
    /* 0x37 */ '*',
    /* 0x38 */ 0, /* Left Alt */
    /* 0x39 */ ' ',
};

static const char shifted_table[SCANCODE_TABLE_SIZE] = {
    /* 0x00 */ 0,
    /* 0x01 */ 27,
    /* 0x02 */ '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    /* 0x0e */ '\b',
    /* 0x0f */ '\t',
    /* 0x10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    /* 0x1c */ '\n',
    /* 0x1d */ 0,
    /* 0x1e */ 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    /* 0x2a */ 0,
    /* 0x2b */ '|',
    /* 0x2c */ 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    /* 0x36 */ 0,
    /* 0x37 */ '*',
    /* 0x38 */ 0,
    /* 0x39 */ ' ',
};

static bool shift_held;

static char kb_storage[256];
static ring_buffer_t kb_buffer;

static trap_frame_t *keyboard_irq_handler(trap_frame_t *frame)
{
    uint8_t scancode = inb(KB_DATA_PORT);

    if (scancode == SCANCODE_LEFT_SHIFT || scancode == SCANCODE_RIGHT_SHIFT) {
        shift_held = true;
    } else if (scancode == (SCANCODE_LEFT_SHIFT | SCANCODE_RELEASE_BIT) ||
               scancode == (SCANCODE_RIGHT_SHIFT | SCANCODE_RELEASE_BIT)) {
        shift_held = false;
    } else if (!(scancode & SCANCODE_RELEASE_BIT) && scancode < SCANCODE_TABLE_SIZE) {
        char c = shift_held ? shifted_table[scancode] : unshifted_table[scancode];
        if (c != 0) {
            ring_buffer_push(&kb_buffer, c);
        }
    }

    return frame;
}

void keyboard_init(void)
{
    ring_buffer_init(&kb_buffer, kb_storage, sizeof(kb_storage));
    irq_register_handler(1, keyboard_irq_handler);
}

bool keyboard_has_char(void)
{
    return !ring_buffer_is_empty(&kb_buffer);
}

char keyboard_getc(void)
{
    char c = 0;
    ring_buffer_pop(&kb_buffer, &c);
    return c;
}
