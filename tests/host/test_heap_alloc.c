/* Host-compiled unit test for libk/heap_alloc.c. Build/run directly:
     gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
         test_heap_alloc.c ../../libk/heap_alloc.c -o test_heap_alloc \
         && ./test_heap_alloc
*/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../libk/heap_alloc.h"

static int checks_failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            checks_failed++; \
        } \
    } while (0)

static void test_basic_alloc_distinct_and_writable(void)
{
    static uint8_t region[4096];
    heap_alloc_t heap;
    heap_alloc_init(&heap, region, sizeof(region));

    void *a = heap_alloc_alloc(&heap, 64);
    void *b = heap_alloc_alloc(&heap, 128);
    CHECK(a != NULL && b != NULL, "basic allocations should succeed");
    CHECK(a != b, "distinct allocations must not alias");

    memset(a, 0xaa, 64);
    memset(b, 0xbb, 128);
    /* If a and b overlapped, one of these would now read back wrong. */
    uint8_t *pa = a, *pb = b;
    for (size_t i = 0; i < 64; i++) {
        CHECK(pa[i] == 0xaa, "allocation A corrupted (overlap?)");
    }
    for (size_t i = 0; i < 128; i++) {
        CHECK(pb[i] == 0xbb, "allocation B corrupted (overlap?)");
    }
}

static void test_free_then_realloc_reuses_block(void)
{
    static uint8_t region[4096];
    heap_alloc_t heap;
    heap_alloc_init(&heap, region, sizeof(region));

    void *a = heap_alloc_alloc(&heap, 64);
    void *b = heap_alloc_alloc(&heap, 64);
    CHECK(a != NULL && b != NULL, "allocations should succeed");

    heap_alloc_free(&heap, a);
    void *c = heap_alloc_alloc(&heap, 64);
    CHECK(c == a, "freeing then re-allocating the same size should reuse the block");
}

static void test_coalesces_adjacent_free_blocks(void)
{
    static uint8_t region[4096];
    heap_alloc_t heap;
    heap_alloc_init(&heap, region, sizeof(region));

    /* Three small blocks, then free all three and confirm a subsequent
       allocation larger than any single one succeeds -- only possible
       if the frees actually coalesced back into one big block. */
    void *a = heap_alloc_alloc(&heap, 32);
    void *b = heap_alloc_alloc(&heap, 32);
    void *c = heap_alloc_alloc(&heap, 32);
    CHECK(a != NULL && b != NULL && c != NULL, "small allocations should succeed");

    heap_alloc_free(&heap, a);
    heap_alloc_free(&heap, b);
    heap_alloc_free(&heap, c);

    void *big = heap_alloc_alloc(&heap, 32 * 3 + 64);
    CHECK(big != NULL, "coalesced free space should satisfy a larger allocation");
}

static void test_out_of_memory_returns_null(void)
{
    static uint8_t region[128];
    heap_alloc_t heap;
    heap_alloc_init(&heap, region, sizeof(region));

    void *a = heap_alloc_alloc(&heap, 4096);
    CHECK(a == NULL, "an allocation larger than the whole region must fail, not corrupt memory");
}

static void test_free_null_is_noop(void)
{
    static uint8_t region[128];
    heap_alloc_t heap;
    heap_alloc_init(&heap, region, sizeof(region));
    heap_alloc_free(&heap, NULL); /* must not crash */

    void *a = heap_alloc_alloc(&heap, 16);
    CHECK(a != NULL, "allocator should still work after freeing NULL");
}

int main(void)
{
    test_basic_alloc_distinct_and_writable();
    test_free_then_realloc_reuses_block();
    test_coalesces_adjacent_free_blocks();
    test_out_of_memory_returns_null();
    test_free_null_is_noop();

    if (checks_failed != 0) {
        fprintf(stderr, "%d check(s) failed\n", checks_failed);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
