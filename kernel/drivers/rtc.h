#ifndef KERNEL_DRIVERS_RTC_H
#define KERNEL_DRIVERS_RTC_H

#include <stdint.h>

/* Wall-clock time as read from the MC146818-compatible CMOS RTC.
   Already converted to plain 24-hour binary fields regardless of the
   hardware's configured BCD/12-hour mode -- callers never need to know
   which mode the RTC happened to be in. */
typedef struct {
    uint8_t second; /* 0-59 */
    uint8_t minute; /* 0-59 */
    uint8_t hour;   /* 0-23 */
    uint8_t day;    /* 1-31 */
    uint8_t month;  /* 1-12 */
    uint16_t year;  /* full 4-digit year -- see rtc.c for the century assumption */
} rtc_time_t;

/* Reads the current wall-clock time from the CMOS RTC (ports
   0x70/0x71). Busy-waits for any in-progress hardware update to finish
   (register A's UIP bit) and double-reads to detect and retry a read
   that straddled an update, per the standard CMOS RTC access pattern
   -- avoids torn values (e.g. seconds=59 read alongside a still-old
   minute) without needing an IRQ8-driven interrupt scheme. Pure port
   I/O; safe to call at any point after boot. */
void rtc_read(rtc_time_t *out);

#endif /* KERNEL_DRIVERS_RTC_H */
