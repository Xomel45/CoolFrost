#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

typedef struct _KHEAPBLOCKBM {
    struct _KHEAPBLOCKBM    *next;
    size_t                   size;   /* total usable bytes in this block    */
    size_t                   used;   /* number of used bitmap slots         */
    size_t                   bsize;  /* bytes per bitmap slot               */
    size_t                   lfb;    /* last free bitmap index (hint)       */
} KHEAPBLOCKBM;

typedef struct _KHEAPBM {
    KHEAPBLOCKBM    *fblock;
} KHEAPBM;

void  k_heapBMInit(KHEAPBM *heap);
int   k_heapBMAddBlock(KHEAPBM *heap, void *addr, size_t size, size_t bsize);
void *k_heapBMAlloc(KHEAPBM *heap, size_t size);
void  k_heapBMFree(KHEAPBM *heap, void *ptr);

#endif
