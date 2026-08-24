#ifndef LIBK_RING_BUFFER_H
#define LIBK_RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Fixed-capacity single-producer/single-consumer byte ring buffer.
 * Pure logic, no I/O or hardware dependency -- host-testable
 * (tests/host/test_ring_buffer.c), same rationale as
 * libk/heap_alloc.c. kernel/drivers/keyboard.c is the intended
 * kernel-side user: one IRQ handler pushes, one polling loop pops,
 * which is exactly the SPSC case this is safe for without a lock (the
 * same "justified lock-free structure" reasoning as pit.c's
 * tick_count, just for a queue instead of a counter). head/tail are
 * volatile for that reason -- harmless for the host test, which is
 * single-threaded, but load-bearing for the real concurrent use.
 */
typedef struct {
    char *buffer;
    size_t capacity;
    volatile size_t head; /* next write index (producer) */
    volatile size_t tail; /* next read index (consumer) */
} ring_buffer_t;

void ring_buffer_init(ring_buffer_t *rb, char *buffer, size_t capacity);
bool ring_buffer_is_empty(const ring_buffer_t *rb);

/* Returns false (and drops c) if the buffer is full. */
bool ring_buffer_push(ring_buffer_t *rb, char c);

/* Returns false (leaving *out untouched) if the buffer is empty. */
bool ring_buffer_pop(ring_buffer_t *rb, char *out);

#endif /* LIBK_RING_BUFFER_H */
