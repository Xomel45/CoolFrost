#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

/* ── Constants ──────────────────────────────────────────────────────────── */
#define MAX_TASKS      32
#define TASK_STACK_SZ  8192    /* 8 KB per task (also used as a ring3 task's ring0 trap stack) */

#define MAX_USER_TASKS 8       /* static pool of ring3 stacks, see cpu/sched.c */
#define USER_STACK_SZ  65536   /* 64 KB ring3 (CPL3) stack, lives in .user_bss */

#define PRIO_REALTIME  0       /* highest — interrupts only */
#define PRIO_HIGH      1
#define PRIO_NORMAL    2       /* default for user tasks   */
#define PRIO_IDLE_LVL  3       /* lowest — background work */
#define PRIO_LEVELS    4

/* Quanta in timer ticks (1000 Hz → 1 ms/tick) */
#define QUANTUM_REALTIME  5
#define QUANTUM_HIGH     10
#define QUANTUM_NORMAL   20
#define QUANTUM_IDLE_LVL 50

/* ── Task states ────────────────────────────────────────────────────────── */
typedef enum {
    TASK_FREE    = 0,   /* slot available for reuse */
    TASK_READY   = 1,   /* in a run queue, waiting for CPU */
    TASK_RUNNING = 2,   /* currently on CPU */
    TASK_BLOCKED = 3,   /* waiting on an event */
    TASK_DONE    = 4,   /* finished, slot eligible for reuse */
} task_state_t;

/* ── Task descriptor ────────────────────────────────────────────────────── */
typedef struct {
    uint64_t      rsp;          /* saved stack pointer (when not running) */
    int           id;
    char          name[24];
    task_state_t  state;
    uint8_t       priority;     /* PRIO_REALTIME … PRIO_IDLE_LVL */
    int           cpu_id;       /* which CPU last ran this task  */
    uint8_t      *stack;        /* heap-allocated stack base (NULL = idle).
                                  * For a ring3 task this is its ring0 trap
                                  * stack — the CPU lands here on any trap
                                  * while the task runs in CPL3 (TSS.RSP0). */
    uint32_t      timeslice;    /* ticks remaining this quantum  */
    uint32_t      quantum;      /* full quantum (ticks)           */
    void        (*func)(void *);
    void         *arg;
    uint8_t       is_user;      /* 1 = runs in ring3 (see sched_submit_user) */
    uint8_t      *user_stack;   /* CPL3 stack (from the .user_bss pool), NULL for ring0 tasks */
    uint64_t      cr3;          /* 0 = shared/master address space (kernel tasks, WM, smoke
                                  * tests); nonzero = this task's private PML4 physical
                                  * address (cpu/vmm.h), loaded by sched_irq_end() before it
                                  * runs — see sched_submit_user_as(). */
} task_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Call once after irq_install(), before the shell loop */
void    sched_init(void);

/* Enable preemption; current thread becomes the idle task */
void    sched_run(void);

/* Submit a task at PRIO_NORMAL (3-arg compat with old API) */
int     sched_submit(const char *name, void (*func)(void *), void *arg);

/* Submit a task at an explicit priority */
int     sched_submit_prio(const char *name, void (*func)(void *), void *arg,
                          uint8_t priority);

/* Submit a task that runs in ring3 (CPL=3), at PRIO_NORMAL. `entry` must be
 * linked into .user_text (see linker.ld); its stack comes from the static
 * .user_bss pool, capped at MAX_USER_TASKS concurrent/ever-submitted tasks.
 * Returns -1 if the pool is exhausted (same non-freeing, slot-bump policy
 * as sched_submit_prio's kernel stacks — fine for the handful of long-lived
 * tasks this scheduler is meant for). */
int     sched_submit_user(const char *name, void (*entry)(void *), void *arg);

/* Submit a task that runs in ring3 with its OWN private address space
 * (cpu/vmm.h) instead of the shared/master one — used by kernel/elf.c for
 * exec'd processes. `cr3` is the process's PML4 physical address
 * (vmm_as_t.pml4_phys); `user_stack_top` is a page the caller already
 * mapped into that same address space (vmm_map_page), NOT the shared
 * .user_stack_pool sched_submit_user() uses. sched_irq_end() reloads CR3
 * around this task automatically. */
int     sched_submit_user_as(const char *name, void (*entry)(void *), void *arg,
                             uint64_t cr3, uint64_t user_stack_top);

/* Voluntarily give up the CPU */
void    sched_yield(void);

/* Same effect as sched_yield, minus the `int $0x20` — for callers already
 * inside an interrupt/syscall frame whose own epilogue will invoke
 * sched_irq_end momentarily (cpu/syscall.c: SYS_YIELD). Calling sched_yield
 * itself there would nest a second interrupt frame for no reason. */
void    sched_want_reschedule(void);

/* Called by a task when it finishes (task_trampoline calls this) */
void    sched_exit(void) __attribute__((noreturn));

/* Called from isr_handler when the faulting context was ring3 (CS RPL==3):
 * marks the current task DONE so sched_irq_end (now also invoked from the
 * ISR path, not just IRQ) switches away from it instead of resuming a
 * corrupted context. Kernel-mode (RPL==0) faults are unaffected. */
void    sched_kill_current(void);

/* Called from timer_callback — flags a reschedule */
void    sched_tick(void);

/* Called from irq_common_stub after irq_handler.
 * Receives current RSP; returns new RSP (may be same or different task). */
uint64_t sched_irq_end(uint64_t cur_rsp);

/* AP idle loop (cooperative fallback for secondary CPUs) */
void    sched_run_loop(int cpu_id);

/* Shell ps command accessors */
int     sched_task_count(void);
task_t *sched_get_task(int idx);

/* Human-readable state string */
const char *sched_state_name(task_state_t s);

#endif
