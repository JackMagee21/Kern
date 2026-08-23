#include <stdint.h>

#include "serial.h"
#include "../../libk/io.h"
#include "../../libk/fmt.h"

/*
 * COM1 16550 UART, base port 0x3f8. Register map (offset from base):
 *   +0 (DLAB=0) data register (RBR/THR)
 *   +0 (DLAB=1) / +1 (DLAB=1) baud rate divisor low/high
 *   +1 (DLAB=0) interrupt enable register (IER)
 *   +2          FIFO control register (FCR)
 *   +3          line control register (LCR); bit 7 = DLAB
 *   +4          modem control register (MCR)
 *   +5          line status register (LSR); bit 5 = THR empty
 * Standard NS16450/16550 layout, matching the raw port I/O used by
 * kernel/arch/x86_64/boot.asm's pre-long-mode panic path.
 */

#define COM1_PORT 0x3f8

#define REG_DATA (COM1_PORT + 0)
#define REG_IER  (COM1_PORT + 1)
#define REG_FCR  (COM1_PORT + 2)
#define REG_LCR  (COM1_PORT + 3)
#define REG_MCR  (COM1_PORT + 4)
#define REG_LSR  (COM1_PORT + 5)

#define LSR_THR_EMPTY 0x20

void serial_init(void)
{
    outb(REG_IER, 0x00);        /* disable all UART interrupts */

    outb(REG_LCR, 0x80);        /* enable DLAB to set the baud divisor */
    outb(REG_DATA, 0x03);       /* divisor low byte: 115200 / 3 = 38400 baud */
    outb(REG_IER, 0x00);        /* divisor high byte */

    outb(REG_LCR, 0x03);        /* DLAB off, 8 data bits, 1 stop bit, no parity */
    outb(REG_FCR, 0xc7);        /* enable FIFO, clear RX/TX, 14-byte threshold */
    outb(REG_MCR, 0x0b);        /* DTR | RTS | OUT2 */
}

void serial_putc(char c)
{
    while ((inb(REG_LSR) & LSR_THR_EMPTY) == 0) {
        /* wait for the transmit holding register to empty */
    }
    outb(REG_DATA, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s != '\0') {
        serial_putc(*s);
        s++;
    }
}

void serial_write_hex(uint64_t value)
{
    char buf[17];
    u64_to_hex(value, buf);
    serial_write(buf);
}
