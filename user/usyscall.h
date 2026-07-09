#ifndef USYSCALL_H
#define USYSCALL_H

#include <stdint.h>
#include "../cpu/syscall.h"

/* int 0x80 wrappers for ring3 (.user_text) callers. always_inline so these
 * never exist as a standalone call target at any optimization level — the
 * `section` tag is a belt-and-suspenders fallback in case they ever do
 * (e.g. address taken): even then they'd still land in .user_text and not
 * fault on the very first instruction fetch. */
__attribute__((always_inline, section(".user_text")))
static inline uint64_t usyscall0(uint64_t num) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

__attribute__((always_inline, section(".user_text")))
static inline uint64_t usyscall1(uint64_t num, uint64_t a1) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "D"(a1) : "memory");
    return ret;
}

#endif
