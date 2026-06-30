#ifndef MATH_H
#define MATH_H

#include <stdint.h>

/* ── Macros (work for any type) ─────────────────────────────────────────── */
#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define ABS(x)             ((x) < 0 ? -(x) : (x))
#define CLAMP(v, lo, hi)   ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define SIGN(x)            ((x) > 0 ? 1 : ((x) < 0 ? -1 : 0))
#define DIV_CEIL(a, b)     (((a) + (b) - 1) / (b))
#define SWAP(T, a, b)      do { T _t = (a); (a) = (b); (b) = _t; } while (0)

/* ── Typed inline functions ─────────────────────────────────────────────── */

static inline int32_t  abs32(int32_t x)  { return x < 0 ? -x : x; }
static inline int64_t  abs64(int64_t x)  { return x < 0 ? -x : x; }

static inline int32_t  min32(int32_t a, int32_t b)  { return a < b ? a : b; }
static inline int32_t  max32(int32_t a, int32_t b)  { return a > b ? a : b; }
static inline int64_t  min64(int64_t a, int64_t b)  { return a < b ? a : b; }
static inline int64_t  max64(int64_t a, int64_t b)  { return a > b ? a : b; }
static inline uint32_t umin32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t umax32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline uint64_t umin64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t umax64(uint64_t a, uint64_t b) { return a > b ? a : b; }

static inline int64_t  clamp64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline uint64_t uclamp64(uint64_t v, uint64_t lo, uint64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Integer square root (floor) — Newton's method ─────────────────────── */
static inline uint64_t isqrt(uint64_t n) {
    if (n == 0) return 0;
    uint64_t x = n;
    uint64_t y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

/* ── Floor log2 of a 32-bit value (undefined for n == 0) ───────────────── */
static inline int log2i(uint32_t n) {
    int r = 0;
    while (n >>= 1) r++;
    return r;
}

/* ── Smallest power of 2 >= n (for 0 < n <= 2^31) ─────────────────────── */
static inline uint32_t next_pow2(uint32_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

/* ── Integer absolute difference ────────────────────────────────────────── */
static inline uint64_t udiff(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}

/* ── Safe multiply — returns 0 and sets *overflow on wrap ───────────────── */
static inline uint64_t umul_safe(uint64_t a, uint64_t b, int *overflow) {
    if (a != 0 && b > (uint64_t)(-1) / a) { *overflow = 1; return 0; }
    *overflow = 0;
    return a * b;
}

#endif
