#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>

/* NOTE: CoolFrost memcpy has non-standard argument order: (source, dest, n) */
void memcpy(uint8_t *source, uint8_t *dest, size_t nbytes);
void memset(void *dest, uint8_t val, size_t len);
int  memcmp(const void *buf1, const void *buf2, size_t count);
void *memmove(void *dest, const void *src, size_t n);

/* Simple bump allocator — no free.  Returns uintptr_t for 64-bit safety. */
uintptr_t kmalloc(size_t size, int align, uintptr_t *phys_addr);

#endif
