#include "fmt.h"

void u64_to_hex(uint64_t value, char out[17])
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 15; i >= 0; i--) {
        out[i] = digits[value & 0xf];
        value >>= 4;
    }
    out[16] = '\0';
}

void u32_to_dec(uint32_t value, char *out)
{
    if (value == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }

    char digits[10];
    int count = 0;
    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    for (int i = 0; i < count; i++) {
        out[i] = digits[count - 1 - i];
    }
    out[count] = '\0';
}
