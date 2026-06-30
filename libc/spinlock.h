#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "atomic.h"

/* ── Ticket spinlock ────────────────────────────────────────────────────── *
 *                                                                            *
 * Guarantees FIFO ordering: threads take turns in the order they called     *
 * spinlock_lock(), so no thread starves.                                     *
 *                                                                            *
 * Usage:                                                                     *
 *   spinlock_t lock = SPINLOCK_INIT;                                         *
 *   spinlock_lock(&lock);                                                    *
 *   // ... critical section ...                                              *
 *   spinlock_unlock(&lock);                                                  */

typedef struct {
    volatile uint16_t next;   /* next ticket to hand out     */
    volatile uint16_t owner;  /* ticket of current owner     */
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spinlock_init(spinlock_t *l) { l->next = 0; l->owner = 0; }

static inline void spinlock_lock(spinlock_t *l) {
    uint16_t ticket;
    /* Atomically fetch-and-increment l->next, get our ticket */
    __asm__ volatile(
        "lock xaddw %0, %1"
        : "=r"(ticket), "+m"(l->next)
        : "0"((uint16_t)1)
        : "memory", "cc"
    );
    /* Spin until it's our turn */
    while (l->owner != ticket) cpu_relax();
}

static inline void spinlock_unlock(spinlock_t *l) {
    cmb();          /* compiler barrier: all prior stores must be visible */
    l->owner++;     /* release: hand lock to the next ticket holder       */
}

/* Non-blocking attempt; returns 1 on success, 0 if already held */
static inline int spinlock_trylock(spinlock_t *l) {
    uint16_t cur_next  = l->next;
    uint16_t cur_owner = l->owner;
    if (cur_next != cur_owner) return 0;   /* someone holds it */
    /* Try to take the single available slot */
    uint16_t old = atomic_cmpxchg32(
        (volatile int32_t *)l,              /* treat both fields as one 32-bit word */
        (int32_t)((cur_next << 16) | cur_owner),
        (int32_t)(((cur_next + 1) << 16) | cur_owner)
    );
    return (int32_t)old == (int32_t)((cur_next << 16) | cur_owner);
}

static inline int spinlock_is_locked(const spinlock_t *l) {
    return l->next != l->owner;
}

/* ── IRQ-safe variants ──────────────────────────────────────────────────── *
 * Save/restore RFLAGS around lock so interrupts stay consistent.            */

static inline uint64_t _save_flags(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return flags;
}

static inline void _restore_flags(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

#define spinlock_lock_irqsave(lock, flags) \
    do { (flags) = _save_flags(); __asm__("cli"); spinlock_lock(lock); } while (0)

#define spinlock_unlock_irqrestore(lock, flags) \
    do { spinlock_unlock(lock); _restore_flags(flags); } while (0)

#endif
