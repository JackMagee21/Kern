#include <stdbool.h>

#include "rtc.h"
#include "../../libk/io.h"

/*
 * MC146818-compatible CMOS RTC. Ports, register indices, and status-
 * register bit layout verified against the OSDev.org "CMOS" wiki
 * article's documentation of this exact, decades-stable hardware
 * interface -- the same class of source this codebase already treats
 * as authoritative for legacy register layouts (ADR 0005's PIC/PIT,
 * ADR 0013's PCI config space).
 */
#define CMOS_ADDRESS 0x70u
#define CMOS_DATA    0x71u

#define RTC_REG_SECONDS  0x00u
#define RTC_REG_MINUTES  0x02u
#define RTC_REG_HOURS    0x04u
#define RTC_REG_DAY      0x07u
#define RTC_REG_MONTH    0x08u
#define RTC_REG_YEAR     0x09u
#define RTC_REG_STATUS_A 0x0Au
#define RTC_REG_STATUS_B 0x0Bu

#define RTC_STATUS_A_UPDATE_IN_PROGRESS (1u << 7)
#define RTC_STATUS_B_24_HOUR            (1u << 1)
#define RTC_STATUS_B_BINARY             (1u << 2)

/* Bit 7 of the address port is NMI-disable on real hardware; always
   writing it clear (as every register index here does, all < 0x80)
   leaves NMI enabled, the harmless/default choice -- this driver has
   no reason to ever disable NMI. */
static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDRESS, reg);
    io_wait();
    return inb(CMOS_DATA);
}

typedef struct {
    uint8_t second, minute, hour, day, month, year;
} raw_rtc_fields_t;

static void read_raw(raw_rtc_fields_t *out)
{
    out->second = cmos_read(RTC_REG_SECONDS);
    out->minute = cmos_read(RTC_REG_MINUTES);
    out->hour   = cmos_read(RTC_REG_HOURS);
    out->day    = cmos_read(RTC_REG_DAY);
    out->month  = cmos_read(RTC_REG_MONTH);
    out->year   = cmos_read(RTC_REG_YEAR);
}

static bool raw_fields_equal(const raw_rtc_fields_t *a, const raw_rtc_fields_t *b)
{
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour
        && a->day == b->day && a->month == b->month && a->year == b->year;
}

static void wait_not_updating(void)
{
    /* The update cycle is ~244us on real hardware -- negligible to
       busy-wait out, and this only ever runs at boot/on an explicit
       `date` command, never in a hot path. */
    while (cmos_read(RTC_REG_STATUS_A) & RTC_STATUS_A_UPDATE_IN_PROGRESS) {
    }
}

static uint8_t bcd_to_binary(uint8_t value)
{
    return (uint8_t)((value & 0x0Fu) + ((value >> 4) * 10u));
}

void rtc_read(rtc_time_t *out)
{
    raw_rtc_fields_t first, second;

    /* Read twice, retrying until two consecutive reads agree -- the
       standard way to avoid a read that straddled the RTC's own
       update cycle (e.g. seconds already rolled over to a new value
       while minutes/hours were read from before the rollover). */
    do {
        wait_not_updating();
        read_raw(&first);
        wait_not_updating();
        read_raw(&second);
    } while (!raw_fields_equal(&first, &second));

    uint8_t status_b = cmos_read(RTC_REG_STATUS_B);
    uint8_t sec = first.second, min = first.minute, hour = first.hour;
    uint8_t day = first.day, month = first.month, year = first.year;

    bool pm = false;
    if (!(status_b & RTC_STATUS_B_24_HOUR)) {
        /* 12-hour mode: bit 7 of the (still BCD-or-binary-encoded)
           hour is the PM flag -- strip it before decoding so BCD
           decoding below operates on a clean 0-12 value. */
        pm = (hour & 0x80u) != 0;
        hour &= 0x7Fu;
    }

    if (!(status_b & RTC_STATUS_B_BINARY)) {
        sec = bcd_to_binary(sec);
        min = bcd_to_binary(min);
        hour = bcd_to_binary(hour);
        day = bcd_to_binary(day);
        month = bcd_to_binary(month);
        year = bcd_to_binary(year);
    }

    if (!(status_b & RTC_STATUS_B_24_HOUR)) {
        /* Standard 12->24-hour conversion: noon is 12, midnight is 0. */
        if (hour == 12) {
            hour = pm ? 12 : 0;
        } else if (pm) {
            hour = (uint8_t)(hour + 12);
        }
    }

    out->second = sec;
    out->minute = min;
    out->hour = hour;
    out->day = day;
    out->month = month;
    /* RTC_REG_YEAR is a 2-digit value with no separate century
       register on this class of hardware; assuming the 21st century
       is correct for any date this kernel will realistically run on
       and matches every other minimal RTC driver's approach (a
       dedicated CMOS century register exists on some systems at a
       model-specific offset, but isn't part of the standard
       MC146818 register set this driver targets). */
    out->year = (uint16_t)(2000u + year);
}
