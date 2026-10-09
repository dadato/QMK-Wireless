/* 单生产者(ISR) / 单消费者(任务) 环形缓冲，不加锁
 * 用读写指针 + 计数器，容量-1 用于区分满/空
 */
#ifndef BRIDGE_RINGBUF_H
#define BRIDGE_RINGBUF_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t *buf;
    uint16_t size;   /* 实际分配字节数 */
    volatile uint16_t head;  /* 写指针（生产者） */
    volatile uint16_t tail;  /* 读指针（消费者） */
} bridge_ringbuf_t;

void     bridge_ringbuf_init(bridge_ringbuf_t *rb, uint8_t *storage, uint16_t size);
uint16_t bridge_ringbuf_count(const bridge_ringbuf_t *rb);
uint16_t bridge_ringbuf_space(const bridge_ringbuf_t *rb);
bool     bridge_ringbuf_push(bridge_ringbuf_t *rb, uint8_t byte);
bool     bridge_ringbuf_pop(bridge_ringbuf_t *rb, uint8_t *byte);
uint16_t bridge_ringbuf_write(bridge_ringbuf_t *rb, const uint8_t *data, uint16_t len);
void     bridge_ringbuf_flush(bridge_ringbuf_t *rb);

#endif /* BRIDGE_RINGBUF_H */
