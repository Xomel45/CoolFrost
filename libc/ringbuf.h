#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>
#include <stddef.h>

/* ── Byte-oriented power-of-2 ring buffer ───────────────────────────────── *
 *                                                                            *
 * cap must be a power of 2. head/tail are unsigned and wrap naturally;      *
 * masking with (cap-1) gives the slot index.                                *
 *                                                                            *
 * Usage:                                                                     *
 *   uint8_t mem[256];                                                        *
 *   ringbuf_t rb;                                                            *
 *   ringbuf_init(&rb, mem, 256);                                             *
 *   ringbuf_push(&rb, 'A');                                                  *
 *   uint8_t c; ringbuf_pop(&rb, &c);                                        *
 *                                                                            *
 * Or use the static declaration helper:                                      *
 *   RINGBUF_DEF(serial_rx, 256);   // declares rb + backing array           */

typedef struct {
    uint8_t *buf;
    size_t   cap;   /* must be a power of 2 */
    size_t   head;  /* read index */
    size_t   tail;  /* write index */
} ringbuf_t;

static inline void ringbuf_init(ringbuf_t *rb, uint8_t *buf, size_t cap) {
    rb->buf  = buf;
    rb->cap  = cap;
    rb->head = 0;
    rb->tail = 0;
}

static inline int    ringbuf_empty(const ringbuf_t *rb) { return rb->head == rb->tail; }
static inline int    ringbuf_full (const ringbuf_t *rb) { return (rb->tail - rb->head) == rb->cap; }
static inline size_t ringbuf_len  (const ringbuf_t *rb) { return rb->tail - rb->head; }
static inline size_t ringbuf_space(const ringbuf_t *rb) { return rb->cap - (rb->tail - rb->head); }

/* ── Single-byte operations ─────────────────────────────────────────────── */

/* Returns 0 on success, -1 if full */
static inline int ringbuf_push(ringbuf_t *rb, uint8_t byte) {
    if (ringbuf_full(rb)) return -1;
    rb->buf[rb->tail & (rb->cap - 1)] = byte;
    rb->tail++;
    return 0;
}

/* Returns 0 and writes *out on success, -1 if empty */
static inline int ringbuf_pop(ringbuf_t *rb, uint8_t *out) {
    if (ringbuf_empty(rb)) return -1;
    *out = rb->buf[rb->head & (rb->cap - 1)];
    rb->head++;
    return 0;
}

/* Peek without consuming */
static inline int ringbuf_peek(const ringbuf_t *rb, uint8_t *out) {
    if (ringbuf_empty(rb)) return -1;
    *out = rb->buf[rb->head & (rb->cap - 1)];
    return 0;
}

/* Peek at offset k from head (0 = same as peek) */
static inline int ringbuf_peek_at(const ringbuf_t *rb, size_t k, uint8_t *out) {
    if (k >= ringbuf_len(rb)) return -1;
    *out = rb->buf[(rb->head + k) & (rb->cap - 1)];
    return 0;
}

/* ── Bulk operations ────────────────────────────────────────────────────── */

/* Write up to n bytes; returns number actually written */
static inline size_t ringbuf_write(ringbuf_t *rb, const uint8_t *src, size_t n) {
    size_t i;
    for (i = 0; i < n && !ringbuf_full(rb); i++)
        ringbuf_push(rb, src[i]);
    return i;
}

/* Read up to n bytes; returns number actually read */
static inline size_t ringbuf_read(ringbuf_t *rb, uint8_t *dst, size_t n) {
    size_t i;
    for (i = 0; i < n && !ringbuf_empty(rb); i++)
        ringbuf_pop(rb, &dst[i]);
    return i;
}

/* Discard up to n bytes */
static inline size_t ringbuf_discard(ringbuf_t *rb, size_t n) {
    size_t avail = ringbuf_len(rb);
    if (n > avail) n = avail;
    rb->head += n;
    return n;
}

static inline void ringbuf_clear(ringbuf_t *rb) {
    rb->head = rb->tail = 0;
}

/* ── Static declaration helper ──────────────────────────────────────────── *
 * RINGBUF_DEF(name, size) — size must be a power-of-2 literal              */
#define RINGBUF_DEF(name, size)                              \
    static uint8_t  _##name##_mem[size];                     \
    static ringbuf_t name = { _##name##_mem, (size), 0, 0 }

#endif
