#include "pit.h"
#include "../../libk/io.h"

/*
 * 8253/8254 PIT, channel 0, ports and base frequency verified against
 * Linux's own definitions (arch/x86/include/asm/i8253.h /
 * include/linux/timex.h: PIT_TICK_RATE = 1193182). Command byte 0x36 =
 * channel 0 (00) | lobyte/hibyte access (11) | mode 3 square wave (011)
 * | binary (0) -- the canonical "16-bit binary counter, mode 3" command
 * cited throughout PIT documentation.
 */
#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND        0x43

#define PIT_BASE_FREQUENCY_HZ 1193182u
#define PIT_MODE3_SQUARE_WAVE 0x36

static volatile uint64_t tick_count;

void pit_init(uint32_t frequency_hz)
{
    uint32_t divisor = PIT_BASE_FREQUENCY_HZ / frequency_hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xffff) {
        divisor = 0xffff; /* 16-bit reload register; 0 would mean 65536, not needed at our frequencies */
    }

    outb(PIT_COMMAND, PIT_MODE3_SQUARE_WAVE);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xff));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xff));
}

void pit_tick(void)
{
    tick_count++;
}

uint64_t pit_get_ticks(void)
{
    return tick_count;
}
