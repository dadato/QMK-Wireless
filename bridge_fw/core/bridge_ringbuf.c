#include "bridge_ringbuf.h"

void bridge_ringbuf_init(bridge_ringbuf_t *rb, uint8_t *storage, uint16_t size)
{
    rb->buf  = storage;
    rb->size = (size > 0u) ? size : 0u;
    rb->head = 0u;
    rb->tail = 0u;
}

uint16_t bridge_ringbuf_count(const bridge_ringbuf_t *rb)
{
    if (rb->size == 0u) {
        return 0u;
    }
    uint16_t h = rb->head;
    uint16_t t = rb->tail;
    return (uint16_t)((h >= t) ? (h - t) : (rb->size - t + h));
}

uint16_t bridge_ringbuf_space(const bridge_ringbuf_t *rb)
{
    if (rb->size == 0u) {
        return 0u;
    }
    return (uint16_t)(rb->size - 1u - bridge_ringbuf_count(rb));
}

bool bridge_ringbuf_push(bridge_ringbuf_t *rb, uint8_t byte)
{
    if (rb->size == 0u) {
        return false;
    }
    uint16_t next = (uint16_t)((rb->head + 1u) % rb->size);
    if (next == rb->tail) {
        return false; /* 满 */
    }
    rb->buf[rb->head] = byte;
    rb->head          = next;
    return true;
}

bool bridge_ringbuf_pop(bridge_ringbuf_t *rb, uint8_t *byte)
{
    if (rb->size == 0u || rb->head == rb->tail) {
        return false; /* 空 */
    }
    *byte    = rb->buf[rb->tail];
    rb->tail = (uint16_t)((rb->tail + 1u) % rb->size);
    return true;
}

uint16_t bridge_ringbuf_write(bridge_ringbuf_t *rb, const uint8_t *data, uint16_t len)
{
    uint16_t written = 0u;
    while (written < len) {
        if (!bridge_ringbuf_push(rb, data[written])) {
            break;
        }
        written++;
    }
    return written;
}

void bridge_ringbuf_flush(bridge_ringbuf_t *rb)
{
    rb->head = 0u;
    rb->tail = 0u;
}
