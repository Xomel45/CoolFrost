#ifndef USERLAND_USYSCALL_H
#define USERLAND_USYSCALL_H

#include <stdint.h>

/* Standalone copy of the int 0x80 ABI (cpu/syscall.h) for programs built
 * outside kernel.elf entirely (see userland/user.ld) — deliberately not
 * shared via #include, this is "userland headers" vs "kernel headers",
 * they're never the same file even when the numbers have to match. */
#define SYS_EXIT  0
#define SYS_WRITE 7

static inline uint64_t usyscall0(uint64_t num) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline uint64_t usyscall2(uint64_t num, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2) : "memory");
    return ret;
}

#endif
