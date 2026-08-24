#include "ring_buffer.h"

void ring_buffer_init(ring_buffer_t *rb, char *buffer, size_t capacity)
{
    rb->buffer = buffer;
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
}

bool ring_buffer_is_empty(const ring_buffer_t *rb)
{
    return rb->head == rb->tail;
}

bool ring_buffer_push(ring_buffer_t *rb, char c)
{
    size_t next = (rb->head + 1) % rb->capacity;
    if (next == rb->tail) {
        return false; /* full */
    }
    rb->buffer[rb->head] = c;
    rb->head = next;
    return true;
}

bool ring_buffer_pop(ring_buffer_t *rb, char *out)
{
    if (rb->head == rb->tail) {
        return false; /* empty */
    }
    *out = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % rb->capacity;
    return true;
}
