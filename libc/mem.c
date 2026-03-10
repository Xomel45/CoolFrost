#include "mem.h"

/* NOTE: non-standard argument order: source first, dest second */
void memcpy(uint8_t *source, uint8_t *dest, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++)
        dest[i] = source[i];
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

/* Bump allocator — pointer grows upward, no free.
 * 64-bit safe: uses uintptr_t throughout. */
static uintptr_t free_mem_addr = 0x10000;

uintptr_t kmalloc(size_t size, int align, uintptr_t *phys_addr) {
    if (align && (free_mem_addr & 0xFFF)) {
        /* Round up to next 4 KiB page boundary */
        free_mem_addr &= ~(uintptr_t)0xFFF;
        free_mem_addr += 0x1000;
    }
    if (phys_addr) *phys_addr = free_mem_addr;
    uintptr_t ret = free_mem_addr;
    free_mem_addr += (uintptr_t)size;
    return ret;
}
