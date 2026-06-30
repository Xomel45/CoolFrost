#ifndef ATOMIC_H
#define ATOMIC_H

#include <stdint.h>

/* ── x86_64 atomic operations ───────────────────────────────────────────── *
 *                                                                            *
 * All operations that need atomicity use the LOCK prefix.                   *
 * All operations that modify memory include a compiler barrier              *
 * ("memory" clobber) to prevent reordering around them.                     */

/* ── Memory barriers ────────────────────────────────────────────────────── */

static inline void mb(void)  { __asm__ volatile("mfence" ::: "memory"); }
static inline void rmb(void) { __asm__ volatile("lfence" ::: "memory"); }
static inline void wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static inline void cmb(void) { __asm__ volatile(""       ::: "memory"); }  /* compiler-only */

/* ── 32-bit atomics ─────────────────────────────────────────────────────── */

static inline int32_t atomic_read32(const volatile int32_t *p) { return *p; }
static inline void    atomic_write32(volatile int32_t *p, int32_t v) {
    __asm__ volatile("movl %1,%0" : "=m"(*p) : "r"(v) : "memory");
}

static inline void atomic_inc32(volatile int32_t *p) {
    __asm__ volatile("lock incl %0" : "+m"(*p) :: "cc", "memory");
}
static inline void atomic_dec32(volatile int32_t *p) {
    __asm__ volatile("lock decl %0" : "+m"(*p) :: "cc", "memory");
}
static inline void atomic_add32(volatile int32_t *p, int32_t v) {
    __asm__ volatile("lock addl %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}
static inline void atomic_sub32(volatile int32_t *p, int32_t v) {
    __asm__ volatile("lock subl %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}
static inline void atomic_and32(volatile int32_t *p, int32_t v) {
    __asm__ volatile("lock andl %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}
static inline void atomic_or32(volatile int32_t *p, int32_t v) {
    __asm__ volatile("lock orl  %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}

/* Returns the old value */
static inline int32_t atomic_xchg32(volatile int32_t *p, int32_t v) {
    __asm__ volatile("xchgl %0,%1" : "+r"(v), "+m"(*p) :: "memory");
    return v;
}

/* Returns the old value; atomically: if *p==old write new */
static inline int32_t atomic_cmpxchg32(volatile int32_t *p, int32_t old, int32_t nw) {
    __asm__ volatile("lock cmpxchgl %2,%1"
                     : "=a"(old), "+m"(*p)
                     : "r"(nw), "0"(old)
                     : "cc", "memory");
    return old;
}

/* Atomic fetch-and-add; returns the value BEFORE the add */
static inline int32_t atomic_fetch_add32(volatile int32_t *p, int32_t v) {
    __asm__ volatile("lock xaddl %0,%1" : "+r"(v), "+m"(*p) :: "cc", "memory");
    return v;
}

/* Decrement and test: returns 1 if result is zero */
static inline int atomic_dec_and_test32(volatile int32_t *p) {
    uint8_t z;
    __asm__ volatile("lock decl %0; setz %1" : "+m"(*p), "=q"(z) :: "cc", "memory");
    return (int)z;
}

/* ── 64-bit atomics ─────────────────────────────────────────────────────── */

static inline int64_t atomic_read64(const volatile int64_t *p) { return *p; }
static inline void    atomic_write64(volatile int64_t *p, int64_t v) {
    __asm__ volatile("movq %1,%0" : "=m"(*p) : "r"(v) : "memory");
}

static inline void atomic_inc64(volatile int64_t *p) {
    __asm__ volatile("lock incq %0" : "+m"(*p) :: "cc", "memory");
}
static inline void atomic_dec64(volatile int64_t *p) {
    __asm__ volatile("lock decq %0" : "+m"(*p) :: "cc", "memory");
}
static inline void atomic_add64(volatile int64_t *p, int64_t v) {
    __asm__ volatile("lock addq %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}

static inline int64_t atomic_xchg64(volatile int64_t *p, int64_t v) {
    __asm__ volatile("xchgq %0,%1" : "+r"(v), "+m"(*p) :: "memory");
    return v;
}

static inline int64_t atomic_cmpxchg64(volatile int64_t *p, int64_t old, int64_t nw) {
    __asm__ volatile("lock cmpxchgq %2,%1"
                     : "=a"(old), "+m"(*p)
                     : "r"(nw), "0"(old)
                     : "cc", "memory");
    return old;
}

static inline int64_t atomic_fetch_add64(volatile int64_t *p, int64_t v) {
    __asm__ volatile("lock xaddq %0,%1" : "+r"(v), "+m"(*p) :: "cc", "memory");
    return v;
}

/* ── Pointer atomics (wrappers around 64-bit) ───────────────────────────── */

static inline void *atomic_read_ptr(void * volatile *p) {
    return (void *)atomic_read64((volatile int64_t *)p);
}
static inline void atomic_write_ptr(void * volatile *p, void *v) {
    atomic_write64((volatile int64_t *)p, (int64_t)v);
}
static inline void *atomic_cmpxchg_ptr(void * volatile *p, void *old, void *nw) {
    return (void *)atomic_cmpxchg64((volatile int64_t *)p,
                                    (int64_t)old, (int64_t)nw);
}

/* ── PAUSE — hint for spin loops ────────────────────────────────────────── */
static inline void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }

#endif
