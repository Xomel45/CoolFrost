#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>
#include <stddef.h>

/* ── Flat bit array — 64 bits per word ──────────────────────────────────── *
 *                                                                            *
 * All functions take a pointer to the first word and the total bit count.   *
 * Extra bits in the last word must be kept zero for count/find to work.     *
 *                                                                            *
 * Usage:                                                                     *
 *   BITMAP_DEF(page_map, 1024);  // 1024-bit bitmap (16 words)              *
 *   bitmap_zero(page_map, 1024); // clear all                               *
 *   bitmap_set(page_map, 5);     // mark page 5 used                        *
 *   size_t free = bitmap_find_first_zero(page_map, 1024);                   */

#define BITMAP_WORDS(n_bits)   (((n_bits) + 63u) / 64u)
#define BITMAP_DEF(name, n)    uint64_t name[BITMAP_WORDS(n)]

/* ── Bit-level operations ───────────────────────────────────────────────── */

static inline void bitmap_set(uint64_t *bm, size_t bit) {
    bm[bit >> 6] |= (1ULL << (bit & 63));
}
static inline void bitmap_clr(uint64_t *bm, size_t bit) {
    bm[bit >> 6] &= ~(1ULL << (bit & 63));
}
static inline int bitmap_test(const uint64_t *bm, size_t bit) {
    return !!(bm[bit >> 6] & (1ULL << (bit & 63)));
}
static inline void bitmap_toggle(uint64_t *bm, size_t bit) {
    bm[bit >> 6] ^= (1ULL << (bit & 63));
}

/* ── Range operations ───────────────────────────────────────────────────── */

static inline void bitmap_set_range(uint64_t *bm, size_t start, size_t len) {
    for (size_t i = start; i < start + len; i++) bitmap_set(bm, i);
}
static inline void bitmap_clr_range(uint64_t *bm, size_t start, size_t len) {
    for (size_t i = start; i < start + len; i++) bitmap_clr(bm, i);
}

/* ── Bulk init ──────────────────────────────────────────────────────────── */

static inline void bitmap_zero(uint64_t *bm, size_t n_bits) {
    size_t w = BITMAP_WORDS(n_bits);
    for (size_t i = 0; i < w; i++) bm[i] = 0;
}
static inline void bitmap_fill(uint64_t *bm, size_t n_bits) {
    size_t w = BITMAP_WORDS(n_bits);
    for (size_t i = 0; i < w; i++) bm[i] = ~0ULL;
    /* Clear the padding bits in the last word */
    size_t tail = n_bits & 63;
    if (tail) bm[w - 1] = (1ULL << tail) - 1;
}

/* ── Search ─────────────────────────────────────────────────────────────── */

/* Returns the index of the first zero bit, or n_bits if all set */
static inline size_t bitmap_find_first_zero(const uint64_t *bm, size_t n_bits) {
    size_t w = BITMAP_WORDS(n_bits);
    for (size_t i = 0; i < w; i++) {
        uint64_t inv = ~bm[i];
        if (!inv) continue;
        size_t bit = i * 64 + (size_t)__builtin_ctzll(inv);
        return bit < n_bits ? bit : n_bits;
    }
    return n_bits;
}

/* Returns the index of the first set bit, or n_bits if all clear */
static inline size_t bitmap_find_first_set(const uint64_t *bm, size_t n_bits) {
    size_t w = BITMAP_WORDS(n_bits);
    for (size_t i = 0; i < w; i++) {
        if (!bm[i]) continue;
        size_t bit = i * 64 + (size_t)__builtin_ctzll(bm[i]);
        return bit < n_bits ? bit : n_bits;
    }
    return n_bits;
}

/* Find first zero bit at or after 'start' */
static inline size_t bitmap_find_next_zero(const uint64_t *bm, size_t n_bits, size_t start) {
    for (size_t bit = start; bit < n_bits; bit++)
        if (!bitmap_test(bm, bit)) return bit;
    return n_bits;
}

/* Find first set bit at or after 'start' */
static inline size_t bitmap_find_next_set(const uint64_t *bm, size_t n_bits, size_t start) {
    for (size_t bit = start; bit < n_bits; bit++)
        if (bitmap_test(bm, bit)) return bit;
    return n_bits;
}

/* Find a contiguous run of 'run' zero bits; returns start index or n_bits */
static inline size_t bitmap_find_free_run(const uint64_t *bm, size_t n_bits, size_t run) {
    size_t start = 0, len = 0;
    for (size_t i = 0; i < n_bits; i++) {
        if (!bitmap_test(bm, i)) {
            if (len == 0) start = i;
            if (++len == run) return start;
        } else {
            len = 0;
        }
    }
    return n_bits;
}

/* ── Count ──────────────────────────────────────────────────────────────── */

static inline size_t bitmap_count_set(const uint64_t *bm, size_t n_bits) {
    size_t w = BITMAP_WORDS(n_bits), c = 0;
    for (size_t i = 0; i < w; i++) c += (size_t)__builtin_popcountll(bm[i]);
    return c;
}
static inline size_t bitmap_count_zero(const uint64_t *bm, size_t n_bits) {
    return n_bits - bitmap_count_set(bm, n_bits);
}

#endif
