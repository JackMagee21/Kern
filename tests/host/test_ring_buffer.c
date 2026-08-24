/* Host-compiled unit test for libk/ring_buffer.c. Build/run directly:
     gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
         test_ring_buffer.c ../../libk/ring_buffer.c -o test_ring_buffer \
         && ./test_ring_buffer
*/
#include <stdio.h>

#include "../../libk/ring_buffer.h"

static int checks_failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            checks_failed++; \
        } \
    } while (0)

static void test_empty_initially(void)
{
    char storage[8];
    ring_buffer_t rb;
    ring_buffer_init(&rb, storage, sizeof(storage));

    CHECK(ring_buffer_is_empty(&rb), "freshly initialized buffer should be empty");

    char out = 'x';
    CHECK(!ring_buffer_pop(&rb, &out), "popping an empty buffer should fail");
    CHECK(out == 'x', "a failed pop must not touch *out");
}

static void test_push_pop_order(void)
{
    char storage[8];
    ring_buffer_t rb;
    ring_buffer_init(&rb, storage, sizeof(storage));

    CHECK(ring_buffer_push(&rb, 'a'), "push into non-full buffer should succeed");
    CHECK(ring_buffer_push(&rb, 'b'), "push into non-full buffer should succeed");
    CHECK(ring_buffer_push(&rb, 'c'), "push into non-full buffer should succeed");
    CHECK(!ring_buffer_is_empty(&rb), "buffer with pushed data should not be empty");

    char out;
    CHECK(ring_buffer_pop(&rb, &out) && out == 'a', "FIFO order: first pushed should pop first");
    CHECK(ring_buffer_pop(&rb, &out) && out == 'b', "FIFO order: second");
    CHECK(ring_buffer_pop(&rb, &out) && out == 'c', "FIFO order: third");
    CHECK(ring_buffer_is_empty(&rb), "buffer should be empty after popping everything pushed");
}

static void test_full_buffer_drops(void)
{
    char storage[4]; /* usable capacity is 3: one slot always kept empty to disambiguate full from empty */
    ring_buffer_t rb;
    ring_buffer_init(&rb, storage, sizeof(storage));

    CHECK(ring_buffer_push(&rb, '1'), "push 1 should succeed");
    CHECK(ring_buffer_push(&rb, '2'), "push 2 should succeed");
    CHECK(ring_buffer_push(&rb, '3'), "push 3 should succeed");
    CHECK(!ring_buffer_push(&rb, '4'), "pushing into a full buffer should fail, not corrupt state");

    char out;
    CHECK(ring_buffer_pop(&rb, &out) && out == '1', "full-buffer drop must not have clobbered existing data");
    CHECK(ring_buffer_pop(&rb, &out) && out == '2', "existing data still intact after a dropped push");
    CHECK(ring_buffer_pop(&rb, &out) && out == '3', "existing data still intact after a dropped push");
}

static void test_wraparound(void)
{
    char storage[4];
    ring_buffer_t rb;
    ring_buffer_init(&rb, storage, sizeof(storage));

    /* Push/pop repeatedly past the physical end of the backing array to
       exercise the modulo wraparound, not just a single fill. */
    for (int round = 0; round < 10; round++) {
        CHECK(ring_buffer_push(&rb, (char)('A' + round)), "push should succeed with room available");
        char out;
        CHECK(ring_buffer_pop(&rb, &out) && out == (char)('A' + round),
              "wraparound: popped value should match what was just pushed");
    }
    CHECK(ring_buffer_is_empty(&rb), "buffer should end empty after equal push/pop counts");
}

int main(void)
{
    test_empty_initially();
    test_push_pop_order();
    test_full_buffer_drops();
    test_wraparound();

    if (checks_failed != 0) {
        fprintf(stderr, "%d check(s) failed\n", checks_failed);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
