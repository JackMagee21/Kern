#ifndef KERNEL_DRIVERS_SERIAL_H
#define KERNEL_DRIVERS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_write_hex(uint64_t value);

#endif /* KERNEL_DRIVERS_SERIAL_H */
