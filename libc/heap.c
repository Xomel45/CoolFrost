#include <stdint.h>
#include <stddef.h>
#include "heap.h"

/*
    2014 Leonard Kevin McGuire Jr (www.kmcg3413.net) (kmcg3413@gmail.com)
    2016 Clément Gallet (provided bug fixes)
    Updated: size_t throughout for 64-bit correctness
*/

void k_heapBMInit(KHEAPBM *heap) {
    heap->fblock = 0;
}

int k_heapBMAddBlock(KHEAPBM *heap, void *addr, size_t size, size_t bsize) {
    KHEAPBLOCKBM    *b;
    size_t           bcnt;
    size_t           x;
    uint8_t         *bm;

    b = (KHEAPBLOCKBM *)addr;
    b->size  = size - sizeof(KHEAPBLOCKBM);
    b->bsize = bsize;

    b->next = heap->fblock;
    heap->fblock = b;

    bcnt = b->size / b->bsize;
    bm = (uint8_t *)&b[1];

    /* clear bitmap */
    for (x = 0; x < bcnt; ++x) bm[x] = 0;

    /* reserve room for bitmap itself */
    bcnt = (bcnt / bsize) * bsize < bcnt ? bcnt / bsize + 1 : bcnt / bsize;
    for (x = 0; x < bcnt; ++x) bm[x] = 5;

    b->lfb  = bcnt - 1;
    b->used = bcnt;

    return 1;
}

static uint8_t k_heapBMGetNID(uint8_t a, uint8_t b) {
    uint8_t c;
    for (c = a + 1; c == b || c == 0; ++c);
    return c;
}

void *k_heapBMAlloc(KHEAPBM *heap, size_t size) {
    KHEAPBLOCKBM    *b;
    uint8_t         *bm;
    size_t           bcnt;
    size_t           x, y, z;
    size_t           bneed;
    uint8_t          nid;

    for (b = heap->fblock; b; b = b->next) {
        if (b->size - (b->used * b->bsize) >= size) {
            bcnt  = b->size / b->bsize;
            bneed = (size / b->bsize) * b->bsize < size
                  ? size / b->bsize + 1
                  : size / b->bsize;
            bm = (uint8_t *)&b[1];

            for (x = (b->lfb + 1 >= bcnt ? 0 : b->lfb + 1); x < b->lfb; ++x) {
                if (x >= bcnt) x = 0;

                if (bm[x] == 0) {
                    for (y = 0; bm[x + y] == 0 && y < bneed && (x + y) < bcnt; ++y);

                    if (y == bneed) {
                        nid = k_heapBMGetNID(bm[x - 1], bm[x + y]);
                        for (z = 0; z < y; ++z) bm[x + z] = nid;
                        b->lfb  = (x + bneed) - 2;
                        b->used += y;
                        return (void *)(x * b->bsize + (uintptr_t)&b[1]);
                    }

                    x += (y - 1);
                    continue;
                }
            }
        }
    }

    return 0;
}

void k_heapBMFree(KHEAPBM *heap, void *ptr) {
    KHEAPBLOCKBM    *b;
    uintptr_t        ptroff;
    size_t           bi, x;
    uint8_t         *bm;
    uint8_t          id;
    size_t           max;

    for (b = heap->fblock; b; b = b->next) {
        if ((uintptr_t)ptr > (uintptr_t)b &&
            (uintptr_t)ptr < (uintptr_t)b + sizeof(KHEAPBLOCKBM) + b->size) {
            ptroff = (uintptr_t)ptr - (uintptr_t)&b[1];
            bi = ptroff / b->bsize;
            bm = (uint8_t *)&b[1];
            id = bm[bi];
            max = b->size / b->bsize;
            for (x = bi; bm[x] == id && x < max; ++x) bm[x] = 0;
            b->used -= x - bi;
            return;
        }
    }
}
