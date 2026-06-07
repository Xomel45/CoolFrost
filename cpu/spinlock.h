#ifndef SPINLOCK_H
#define SPINLOCK_H

typedef struct { volatile int val; } spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *s) {
    while (__sync_lock_test_and_set(&s->val, 1))
        while (s->val) asm volatile("pause");
}

static inline void spin_unlock(spinlock_t *s) {
    __sync_lock_release(&s->val);
}

#endif
