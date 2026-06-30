#include "sort.h"
#include <stdint.h>

typedef int (*cmp_t)(const void *, const void *);

/* Byte-level in-place swap — no memcpy dependency */
static void bswap(uint8_t *a, uint8_t *b, size_t sz) {
    for (; sz--; a++, b++) { uint8_t t = *a; *a = *b; *b = t; }
}

/* Median of three — returns pointer to the middle-valued element */
static uint8_t *med3(uint8_t *a, uint8_t *b, uint8_t *c, cmp_t cmp) {
    if (cmp(a, b) < 0) {
        if (cmp(b, c) < 0) return b;
        return cmp(a, c) < 0 ? c : a;
    }
    if (cmp(b, c) > 0) return b;
    return cmp(a, c) > 0 ? c : a;
}

/* Insertion sort — used for small partitions (avoids recursion overhead) */
#define ISORT_MAX 8
static void isort(uint8_t *base, size_t n, size_t sz, cmp_t cmp) {
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(base + (j-1)*sz, base + j*sz) > 0; j--)
            bswap(base + (j-1)*sz, base + j*sz, sz);
}

/* Recursive Lomuto quicksort with median-of-3 pivot */
static void qsort_impl(uint8_t *base, size_t n, size_t sz, cmp_t cmp) {
    if (n < 2) return;
    if (n <= ISORT_MAX) { isort(base, n, sz, cmp); return; }

    /* Choose pivot via median-of-3, swap to last position */
    uint8_t *piv = med3(base, base + (n/2)*sz, base + (n-1)*sz, cmp);
    bswap(piv, base + (n-1)*sz, sz);
    piv = base + (n-1)*sz;

    /* Partition */
    size_t i = 0;
    for (size_t j = 0; j < n - 1; j++) {
        if (cmp(base + j*sz, piv) <= 0)
            bswap(base + i++*sz, base + j*sz, sz);
    }
    bswap(base + i*sz, piv, sz);

    /* Recurse on both halves */
    if (i > 0) qsort_impl(base, i, sz, cmp);
    if (n - i > 2) qsort_impl(base + (i + 1)*sz, n - i - 1, sz, cmp);
}

void ksort(void *base, size_t n, size_t sz, cmp_t cmp) {
    qsort_impl((uint8_t *)base, n, sz, cmp);
}

void *kbsearch(const void *key, const void *base, size_t n, size_t sz, cmp_t cmp) {
    const uint8_t *lo = (const uint8_t *)base;
    size_t hi = n;
    while (hi > 0) {
        size_t mid = hi / 2;
        int r = cmp(key, lo + mid * sz);
        if (r == 0) return (void *)(lo + mid * sz);
        if (r > 0) { lo += (mid + 1) * sz; hi -= mid + 1; }
        else         hi  =  mid;
    }
    return (void *)0;
}
