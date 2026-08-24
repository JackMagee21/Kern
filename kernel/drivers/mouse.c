#include <stdint.h>

#include "mouse.h"
#include "../../libk/io.h"
#include "../arch/x86_64/irq.h"

/*
 * PS/2 mouse: the 8042 controller's second ("auxiliary") port, IRQ12.
 * Ports, controller commands, the configuration-byte bit layout, and
 * the 3-byte streaming packet format verified against the OSDev.org
 * "Mouse Input" wiki article's documentation of this decades-standard
 * interface -- the same class of source this codebase already treats
 * as authoritative for legacy hardware (ADR 0005, ADR 0013, ADR 0014,
 * ADR 0015).
 */
#define KB_DATA_PORT   0x60u
#define KB_STATUS_PORT 0x64u
#define KB_CMD_PORT    0x64u

#define KB_STATUS_OUTPUT_FULL (1u << 0)
#define KB_STATUS_INPUT_FULL  (1u << 1)

#define KB_CMD_ENABLE_AUX_PORT   0xA8u
#define KB_CMD_READ_CONFIG_BYTE  0x20u
#define KB_CMD_WRITE_CONFIG_BYTE 0x60u
#define KB_CMD_WRITE_TO_AUX      0xD4u

#define CONFIG_AUX_IRQ_ENABLE    (1u << 1) /* enables IRQ12 generation for aux-port bytes */
#define CONFIG_AUX_CLOCK_DISABLE (1u << 5) /* must be CLEAR for the mouse to actually clock data out */

#define MOUSE_CMD_SET_DEFAULTS     0xF6u
#define MOUSE_CMD_ENABLE_REPORTING 0xF4u
#define MOUSE_RESP_ACK             0xFAu
#define MOUSE_RESP_RESEND          0xFEu

/* Packet byte 0 (flags) bit layout. Bit 3 is always 1 on a genuine
   first byte -- the standard resync check every PS/2 mouse driver uses
   to recover if a byte gets lost/duplicated. */
#define PACKET_LEFT_BUTTON    (1u << 0)
#define PACKET_RIGHT_BUTTON   (1u << 1)
#define PACKET_MIDDLE_BUTTON  (1u << 2)
#define PACKET_ALWAYS_ONE_BIT (1u << 3)
#define PACKET_X_SIGN         (1u << 4)
#define PACKET_Y_SIGN         (1u << 5)
#define PACKET_X_OVERFLOW     (1u << 6)
#define PACKET_Y_OVERFLOW     (1u << 7)

static void wait_input_clear(void)
{
    while (inb(KB_STATUS_PORT) & KB_STATUS_INPUT_FULL) {
    }
}

static void wait_output_full(void)
{
    while (!(inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL)) {
    }
}

static void controller_write_command(uint8_t cmd)
{
    wait_input_clear();
    outb(KB_CMD_PORT, cmd);
}

static void controller_write_data(uint8_t data)
{
    wait_input_clear();
    outb(KB_DATA_PORT, data);
}

static uint8_t controller_read_data(void)
{
    wait_output_full();
    return inb(KB_DATA_PORT);
}

/* KB_CMD_WRITE_TO_AUX tells the controller "route the next data byte
   to the mouse, not the keyboard" -- every command actually sent TO
   the mouse device itself (as opposed to the controller) goes through
   this prefix. */
static void mouse_write(uint8_t byte)
{
    controller_write_command(KB_CMD_WRITE_TO_AUX);
    controller_write_data(byte);
}

#define EVENT_QUEUE_CAPACITY 32u

static mouse_event_t event_queue[EVENT_QUEUE_CAPACITY];
static volatile uint32_t queue_head; /* next write index (producer: the IRQ handler) */
static volatile uint32_t queue_tail; /* next read index (consumer: whoever calls mouse_get_event) */

/* Single-producer/single-consumer, same reasoning as libk/ring_buffer.c
   -- not reused directly since that module is char-specific and a
   second, struct-typed instantiation isn't worth generalizing it for
   yet (CLAUDE.md: don't add abstractions beyond what's needed). */
static void queue_push(mouse_event_t event)
{
    uint32_t next_head = (queue_head + 1u) % EVENT_QUEUE_CAPACITY;
    if (next_head == queue_tail) {
        return; /* full -- drop, matching keyboard.c's ring buffer's contract */
    }
    event_queue[queue_head] = event;
    queue_head = next_head;
}

static uint8_t packet[3];
static int packet_index;

static trap_frame_t *mouse_irq_handler(trap_frame_t *frame)
{
    uint8_t data = inb(KB_DATA_PORT);

    if (packet_index == 0) {
        if (data == MOUSE_RESP_ACK || data == MOUSE_RESP_RESEND) {
            /* A device-response byte arriving outside the init
               handshake -- observed in practice (ADR 0016's
               Verification): an ACK for the enable-reporting command
               can arrive asynchronously, well after mouse_init()
               returns and drained whatever was already buffered at
               that point, right around when the mouse is first
               actually touched. 0xFA's bit 3 happens to be set, so
               without this explicit check it would pass the framing
               test below and be mistaken for a genuine first packet
               byte, corrupting the next real packet. Never a
               legitimate flags byte's value on its own (would require
               every button plus both overflow bits simultaneously). */
            return frame;
        }
        if (!(data & PACKET_ALWAYS_ONE_BIT)) {
            return frame; /* out of sync -- drop bytes until a real first byte arrives */
        }
    }

    packet[packet_index++] = data;
    if (packet_index < 3) {
        return frame;
    }
    packet_index = 0;

    uint8_t flags = packet[0];
    if (flags & (PACKET_X_OVERFLOW | PACKET_Y_OVERFLOW)) {
        return frame; /* overflow -- the delta isn't meaningful, discard the whole packet */
    }

    /* 9-bit signed value: 8-bit magnitude (packet[1]/packet[2]) plus a
       separate sign bit in the flags byte -- NOT plain two's
       complement, per the PS/2 protocol's own packet format. */
    int16_t dx = (int16_t)packet[1];
    if (flags & PACKET_X_SIGN) {
        dx = (int16_t)(dx - 256);
    }
    int16_t dy = (int16_t)packet[2];
    if (flags & PACKET_Y_SIGN) {
        dy = (int16_t)(dy - 256);
    }

    mouse_event_t event = {
        .dx = dx,
        .dy = dy,
        .left = (flags & PACKET_LEFT_BUTTON) != 0,
        .right = (flags & PACKET_RIGHT_BUTTON) != 0,
        .middle = (flags & PACKET_MIDDLE_BUTTON) != 0,
    };
    queue_push(event);

    return frame;
}

/* Defensive flush: consume any byte already sitting in the
   controller's output buffer before trusting the stream is clean.
   Doesn't catch every case by itself -- diagnosed the hard way (ADR
   0016's Verification section) that the handshake's own ACK (0xFA)
   can still arrive asynchronously well AFTER this returns, which is
   what the explicit MOUSE_RESP_ACK/MOUSE_RESP_RESEND check in
   mouse_irq_handler actually guards against. Kept anyway as cheap,
   standard defense-in-depth for the ordinary case where something IS
   already buffered at this point. Bounded iteration count, not an
   unconditional loop: CLAUDE.md rule 2 (no unbounded waits in a path
   this close to enabling interrupts) -- a genuinely stuck controller
   shouldn't be able to hang boot here. */
static void drain_output_buffer(void)
{
    for (int i = 0; i < 16; i++) {
        if (!(inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL)) {
            return;
        }
        (void)inb(KB_DATA_PORT);
    }
}

void mouse_init(void)
{
    controller_write_command(KB_CMD_ENABLE_AUX_PORT);

    controller_write_command(KB_CMD_READ_CONFIG_BYTE);
    uint8_t config = controller_read_data();
    config |= CONFIG_AUX_IRQ_ENABLE;
    config &= (uint8_t)~CONFIG_AUX_CLOCK_DISABLE;
    controller_write_command(KB_CMD_WRITE_CONFIG_BYTE);
    controller_write_data(config);

    mouse_write(MOUSE_CMD_SET_DEFAULTS);
    (void)controller_read_data(); /* ACK (0xFA) -- not checked, matches this driver's "trust the standard sequence" scope */

    mouse_write(MOUSE_CMD_ENABLE_REPORTING);
    (void)controller_read_data(); /* ACK */

    drain_output_buffer();

    irq_register_handler(12, mouse_irq_handler);
}

bool mouse_has_event(void)
{
    return queue_head != queue_tail;
}

mouse_event_t mouse_get_event(void)
{
    mouse_event_t event = {0};
    if (queue_head == queue_tail) {
        return event;
    }
    event = event_queue[queue_tail];
    queue_tail = (queue_tail + 1u) % EVENT_QUEUE_CAPACITY;
    return event;
}
