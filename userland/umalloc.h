#ifndef USERLAND_UMALLOC_H
#define USERLAND_UMALLOC_H

#include "usyscall.h"

/* A real (if simple) malloc()/free() for userland ELF programs, built
 * entirely on top of SYS_SBRK — the kernel has no notion of individual
 * allocations, only a raw per-process break (cpu/syscall.c: SYS_SBRK,
 * cpu/sched.h: task_t.heap_*). This header does the actual free-list
 * bookkeeping, same split as libc's malloc sitting on top of a kernel's
 * brk()/mmap() in a real OS.
 *
 * Design: first-fit, address-sorted singly-linked free list. Each block
 * (free or allocated) starts with a umalloc_block_t header immediately
 * followed by its usable memory. Growing the heap (umalloc_grow) always
 * asks for at least a page's worth, so small allocations don't each cost a
 * syscall. Freed blocks are only coalesced FORWARD (with the block right
 * after them in address order) — coalescing backward would need a doubly-
 * linked list or a full rescan, skipped as a deliberate simplification;
 * worst case is a free list carrying more, smaller entries than a fully
 * general allocator would, not a correctness problem.
 *
 * Header-only (like usyscall.h) so a program just #includes this — no
 * separate .c to add to the Makefile. `static inline` throughout means
 * each translation unit gets its own copy of the free-list state, which is
 * exactly right here: umalloc.h is only ever used by a single-TU userland
 * ELF (one source file per program under userland/, no linking multiple
 * objects together), never something that could end up with two
 * conflicting copies of the free list in one process.
 */

typedef struct umalloc_block {
    uint64_t              size;   /* usable bytes, NOT including this header */
    struct umalloc_block *next;   /* next block in the free list; meaningless once allocated */
    uint8_t                free;
} umalloc_block_t;

#define UMALLOC_ALIGN     16ULL
#define UMALLOC_HDR_SIZE  ((sizeof(umalloc_block_t) + (UMALLOC_ALIGN - 1)) & ~(UMALLOC_ALIGN - 1))
#define UMALLOC_MIN_GROW  4096ULL   /* don't SYS_SBRK for less than a page at a time */

static umalloc_block_t *umalloc_free_list = (umalloc_block_t *)0;

static inline uint64_t umalloc_align_up(uint64_t x) {
    return (x + (UMALLOC_ALIGN - 1)) & ~(UMALLOC_ALIGN - 1);
}

/* Inserts `blk` into the free list in address order — coalescing in ufree
 * only ever needs to look at blk->next as a result, never rescan the whole
 * list to find "the block right after this one". */
static inline void umalloc_list_insert(umalloc_block_t *blk) {
    if (!umalloc_free_list || blk < umalloc_free_list) {
        blk->next = umalloc_free_list;
        umalloc_free_list = blk;
        return;
    }
    umalloc_block_t *cur = umalloc_free_list;
    while (cur->next && cur->next < blk) cur = cur->next;
    blk->next = cur->next;
    cur->next = blk;
}

/* Asks the kernel for at least `min_size` (plus header) more bytes via
 * SYS_SBRK and returns it as one fresh, NOT-yet-linked-anywhere block.
 * Returns NULL if SYS_SBRK refuses (no heap on this process, or it's full
 * — cpu/sched.h: task_t.heap_end). */
static inline umalloc_block_t *umalloc_grow(uint64_t min_size) {
    uint64_t chunk = min_size + UMALLOC_HDR_SIZE;
    if (chunk < UMALLOC_MIN_GROW) chunk = UMALLOC_MIN_GROW;

    uint64_t base = usyscall1(SYS_SBRK, chunk);
    if (base == (uint64_t)-1) return (umalloc_block_t *)0;

    umalloc_block_t *blk = (umalloc_block_t *)(unsigned long)base;
    blk->size = chunk - UMALLOC_HDR_SIZE;
    blk->free = 1;
    blk->next = (umalloc_block_t *)0;
    return blk;
}

static inline void *umalloc(uint64_t size) {
    if (size == 0) return (void *)0;
    size = umalloc_align_up(size);

    umalloc_block_t *prev = (umalloc_block_t *)0;
    umalloc_block_t *cur  = umalloc_free_list;
    while (cur && !(cur->free && cur->size >= size)) {
        prev = cur;
        cur = cur->next;
    }

    if (cur) {
        /* Found room on the free list — unlink it. */
        if (prev) prev->next = cur->next;
        else      umalloc_free_list = cur->next;
    } else {
        /* Nothing big enough — grow the heap. The new block isn't linked
         * into the free list at all, so there's nothing to unlink here. */
        cur = umalloc_grow(size);
        if (!cur) return (void *)0;
    }

    /* Split off the remainder if there's enough left for it to be worth
     * being its own block later (room for a header plus at least one
     * aligned unit of usable space). */
    if (cur->size >= size + UMALLOC_HDR_SIZE + UMALLOC_ALIGN) {
        umalloc_block_t *rem = (umalloc_block_t *)((uint8_t *)cur + UMALLOC_HDR_SIZE + size);
        rem->size = cur->size - size - UMALLOC_HDR_SIZE;
        rem->free = 1;
        umalloc_list_insert(rem);
        cur->size = size;
    }

    cur->free = 0;
    cur->next = (umalloc_block_t *)0;
    return (void *)((uint8_t *)cur + UMALLOC_HDR_SIZE);
}

static inline void ufree(void *ptr) {
    if (!ptr) return;

    umalloc_block_t *blk = (umalloc_block_t *)((uint8_t *)ptr - UMALLOC_HDR_SIZE);
    blk->free = 1;
    umalloc_list_insert(blk);

    /* Coalesce forward only — see this header's top comment. */
    if (blk->next &&
        (uint8_t *)blk + UMALLOC_HDR_SIZE + blk->size == (uint8_t *)blk->next) {
        umalloc_block_t *nxt = blk->next;
        blk->size += UMALLOC_HDR_SIZE + nxt->size;
        blk->next = nxt->next;
    }
}

#endif
