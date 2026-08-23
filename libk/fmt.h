#ifndef LIBK_FMT_H
#define LIBK_FMT_H

#include <stdint.h>

/* Writes 16 hex digits (zero-padded, lowercase) followed by a NUL into
   out, which must be at least 17 bytes. Pure function, no I/O -- host
   testable (see tests/host/test_fmt.c). */
void u64_to_hex(uint64_t value, char out[17]);

#endif /* LIBK_FMT_H */
