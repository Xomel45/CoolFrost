#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void memset(void *dest, uint8_t val, size_t len);
int  memcmp(const void *buf1, const void *buf2, size_t count);
void *memmove(void *dest, const void *src, size_t n);

/* Kernel heap allocator (bitmap-backed, supports freeing).
 * align != 0 requests 4 KiB page alignment. */
uintptr_t kmalloc(size_t size, int align, uintptr_t *phys_addr);
void kfree(void *ptr);

#endif
