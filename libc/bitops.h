#ifndef BITOPS_H
#define BITOPS_H

#include <stdint.h>
#include <stddef.h>

/* ── Count leading / trailing zeros ─────────────────────────────────────── *
 * Undefined for n == 0.  Use the safe variants (_safe) which return -1.    */

static inline int clz32(uint32_t n) { return __builtin_clz(n); }
static inline int clz64(uint64_t n) { return __builtin_clzll(n); }
static inline int ctz32(uint32_t n) { return __builtin_ctz(n); }
static inline int ctz64(uint64_t n) { return __builtin_ctzll(n); }

static inline int clz32_safe(uint32_t n) { return n ? __builtin_clz(n)   : -1; }
static inline int clz64_safe(uint64_t n) { return n ? __builtin_clzll(n) : -1; }
static inline int ctz32_safe(uint32_t n) { return n ? __builtin_ctz(n)   : -1; }
static inline int ctz64_safe(uint64_t n) { return n ? __builtin_ctzll(n) : -1; }

/* ── Population count ───────────────────────────────────────────────────── */
static inline int popcount32(uint32_t n) { return __builtin_popcount(n); }
static inline int popcount64(uint64_t n) { return __builtin_popcountll(n); }

/* ── Bit scan reverse — index of highest set bit, 0-based ──────────────── *
 * Undefined for n == 0.                                                     */
static inline int bsr32(uint32_t n) { return 31 - __builtin_clz(n); }
static inline int bsr64(uint64_t n) { return 63 - __builtin_clzll(n); }

/* ── Bit set / clear / test / flip ─────────────────────────────────────── */
#define BIT_SET(x, b)    ((x) |=  (1ULL << (b)))
#define BIT_CLR(x, b)    ((x) &= ~(1ULL << (b)))
#define BIT_TST(x, b)    (!!((x) & (1ULL << (b))))
#define BIT_FLP(x, b)    ((x) ^=  (1ULL << (b)))

/* ── Alignment helpers (alignment must be a power of 2) ────────────────── */
#define ALIGN_UP(x, a)      (((uintptr_t)(x) + (a) - 1) & ~((uintptr_t)(a) - 1))
#define ALIGN_DOWN(x, a)    ((uintptr_t)(x) & ~((uintptr_t)(a) - 1))
#define IS_ALIGNED(x, a)    (!((uintptr_t)(x) & ((uintptr_t)(a) - 1)))
#define IS_POW2(n)          ((n) != 0 && !((n) & ((n) - 1)))

/* ── Byte-swap (endian) ─────────────────────────────────────────────────── */
static inline uint16_t bswap16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t bswap64(uint64_t x) { return __builtin_bswap64(x); }

/* ── Extract / deposit bit fields ───────────────────────────────────────── */
/* Extract bits [hi:lo] from x */
#define BITS(x, hi, lo)  (((x) >> (lo)) & ((1ULL << ((hi) - (lo) + 1)) - 1))
/* Mask of n bits starting at bit lo */
#define BITMASK(lo, n)   (((1ULL << (n)) - 1) << (lo))

/* ── Rotate ─────────────────────────────────────────────────────────────── */
static inline uint32_t rol32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}
static inline uint32_t ror32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
static inline uint64_t rol64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}
static inline uint64_t ror64(uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

#endif
