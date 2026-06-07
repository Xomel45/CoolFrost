#include "sched.h"
#include "spinlock.h"
#include "../libc/mem.h"

static task_t      task_list[MAX_TASKS];
static int         task_count = 0;
static spinlock_t  task_lock  = SPINLOCK_INIT;

/* ── Name copy helper (no strncpy in libc) ─────────────────────────────── */
static void copy_name(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int sched_submit(const char *name, void (*func)(void *), void *arg) {
    spin_lock(&task_lock);
    if (task_count >= MAX_TASKS) {
        spin_unlock(&task_lock);
        return -1;
    }
    task_t *t = &task_list[task_count];
    t->id     = task_count++;
    t->state  = TASK_READY;
    t->func   = func;
    t->arg    = arg;
    t->cpu_id = -1;
    copy_name(t->name, name, sizeof(t->name));
    spin_unlock(&task_lock);
    return t->id;
}

/* Pick the next READY task atomically, mark it RUNNING. Returns NULL if none. */
static task_t *pick_task(void) {
    spin_lock(&task_lock);
    for (int i = 0; i < task_count; i++) {
        if (task_list[i].state == TASK_READY) {
            task_list[i].state = TASK_RUNNING;
            spin_unlock(&task_lock);
            return &task_list[i];
        }
    }
    spin_unlock(&task_lock);
    return (task_t *)0;
}

void sched_run_loop(int cpu_id) {
    while (1) {
        task_t *t = pick_task();
        if (t) {
            t->cpu_id = cpu_id;
            t->func(t->arg);
            t->state = TASK_DONE;
        } else {
            asm volatile("hlt");   /* wait for an interrupt / IPI to wake us */
        }
    }
}

int sched_task_count(void) { return task_count; }

task_t *sched_get_task(int idx) {
    if (idx < 0 || idx >= task_count) return (task_t *)0;
    return &task_list[idx];
}
