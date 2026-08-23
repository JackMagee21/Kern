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
