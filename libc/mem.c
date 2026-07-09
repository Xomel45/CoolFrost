#include "mem.h"
#include "heap.h"

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

void memset(void *dest, uint8_t val, size_t len) {
    uint8_t *p = (uint8_t *)dest;
    while (len--) *p++ = val;
}

int memcmp(const void *buf1, const void *buf2, size_t count) {
    if (!count) return 0;
    const uint8_t *a = (const uint8_t *)buf1;
    const uint8_t *b = (const uint8_t *)buf2;
    while (--count && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

void *memmove(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    if (d == s) return dest;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

/* Kernel heap: bitmap allocator (libc/heap.c) over a static BSS arena.
 * Identity-mapped, so virtual == physical.
 *
 * Every allocation is prefixed with a header word holding the raw block
 * pointer, so kfree() works for both plain and page-aligned allocations. */
#define KHEAP_SIZE   (16u * 1024u * 1024u)  /* 16 MiB — fits a 32bpp backbuffer + DE allocations */
#define KHEAP_BSIZE  64u                    /* bytes per bitmap slot */

static uint8_t kheap_area[KHEAP_SIZE] __attribute__((aligned(4096)));
static KHEAPBM kheap;
static int     kheap_ready = 0;

static void kheap_ensure_init(void) {
    if (kheap_ready) return;
    k_heapBMInit(&kheap);
    k_heapBMAddBlock(&kheap, kheap_area, KHEAP_SIZE, KHEAP_BSIZE);
    kheap_ready = 1;
}

uintptr_t kmalloc(size_t size, int align, uintptr_t *phys_addr) {
    kheap_ensure_init();

    /* Room for the back-pointer header + worst-case alignment padding */
    size_t extra = sizeof(uintptr_t) + (align ? 0x1000 : 0);
    uint8_t *raw = (uint8_t *)k_heapBMAlloc(&kheap, size + extra);
    if (!raw) {
        if (phys_addr) *phys_addr = 0;
        return 0;
    }

    uintptr_t p = (uintptr_t)raw + sizeof(uintptr_t);
    if (align)
        p = (p + 0xFFF) & ~(uintptr_t)0xFFF;
    ((uintptr_t *)p)[-1] = (uintptr_t)raw;

    if (phys_addr) *phys_addr = p;   /* identity-mapped */
    return p;
}

void kfree(void *ptr) {
    if (!ptr || !kheap_ready) return;
    uintptr_t raw = ((uintptr_t *)ptr)[-1];
    k_heapBMFree(&kheap, (void *)raw);
}
