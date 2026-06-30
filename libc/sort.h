#ifndef SORT_H
#define SORT_H

#include <stddef.h>

/* ── In-place quicksort ─────────────────────────────────────────────────── *
 *                                                                            *
 * Works on any element type via a size + comparator.                        *
 * Uses median-of-3 pivot and insertion sort for small partitions.           *
 *                                                                            *
 * cmp(a, b) must return < 0, 0, or > 0 (like standard qsort).              */
void ksort(void *base, size_t n, size_t elem_sz,
           int (*cmp)(const void *a, const void *b));

/* ── Binary search ──────────────────────────────────────────────────────── *
 *                                                                            *
 * Searches the sorted array [base, base+n*elem_sz) for key.                 *
 * Returns a pointer to a matching element, or NULL if not found.            *
 * If multiple elements compare equal, which one is returned is unspecified. */
void *kbsearch(const void *key, const void *base, size_t n, size_t elem_sz,
               int (*cmp)(const void *key, const void *elem));

#endif
