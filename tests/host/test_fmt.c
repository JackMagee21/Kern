/* Host-compiled unit test for libk/fmt.c (see CLAUDE.md "Testing":
   host-testable logic gets a host test, run with the host compiler +
   ASan/UBSan -- this file, unlike kernel sources, is meant to be host
   compiled). Build/run directly, e.g.:
     gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
         -I../.. test_fmt.c ../../libk/fmt.c -o test_fmt && ./test_fmt
*/
#include <stdio.h>
#include <string.h>

#include "../../libk/fmt.h"

static int checks_failed = 0;

static void expect_hex(uint64_t value, const char *expected)
{
    char out[17];
    u64_to_hex(value, out);
    if (strcmp(out, expected) != 0) {
        fprintf(stderr, "u64_to_hex(0x%llx) = \"%s\", expected \"%s\"\n",
                (unsigned long long)value, out, expected);
        checks_failed++;
    }
}

static void expect_dec(uint32_t value, const char *expected)
{
    char out[11];
    u32_to_dec(value, out);
    if (strcmp(out, expected) != 0) {
        fprintf(stderr, "u32_to_dec(%u) = \"%s\", expected \"%s\"\n",
                (unsigned)value, out, expected);
        checks_failed++;
    }
}

int main(void)
{
    expect_hex(0x0, "0000000000000000");
    expect_hex(0x1, "0000000000000001");
    expect_hex(0xdeadbeef, "00000000deadbeef");
    expect_hex(0xffffffffffffffffULL, "ffffffffffffffff");
    expect_hex(0x0123456789abcdefULL, "0123456789abcdef");

    expect_dec(0, "0");
    expect_dec(1, "1");
    expect_dec(9, "9");
    expect_dec(10, "10");
    expect_dec(2026, "2026");
    expect_dec(4294967295u, "4294967295");

    if (checks_failed != 0) {
        fprintf(stderr, "%d check(s) failed\n", checks_failed);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
