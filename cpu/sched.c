#include "sched.h"
#include "../libc/mem.h"
#include "gdt.h"
#include "tss.h"

/* ── Quanta per priority level (ticks at 1000 Hz = 1 ms/tick) ──────────── */
static const uint32_t prio_quantum[PRIO_LEVELS] = {
    QUANTUM_REALTIME, QUANTUM_HIGH, QUANTUM_NORMAL, QUANTUM_IDLE_LVL
};

/* ── Task pool ──────────────────────────────────────────────────────────── */
static task_t  task_pool[MAX_TASKS];
static int     task_count = 0;   /* highest slot index ever used + 1 */

/* ── Priority run queues: circular FIFO per level ─────────────────────── */
static task_t *rq_buf [PRIO_LEVELS][MAX_TASKS];
static int     rq_head[PRIO_LEVELS];
static int     rq_tail[PRIO_LEVELS];
static int     rq_size[PRIO_LEVELS];

static void rq_push(int p, task_t *t) {
    if (rq_size[p] >= MAX_TASKS) return;
    rq_buf[p][rq_tail[p]] = t;
    rq_tail[p] = (rq_tail[p] + 1) % MAX_TASKS;
    rq_size[p]++;
}

static task_t *rq_pop(int p) {
    if (rq_size[p] == 0) return (task_t *)0;
    task_t *t = rq_buf[p][rq_head[p]];
    rq_head[p] = (rq_head[p] + 1) % MAX_TASKS;
    rq_size[p]--;
    return t;
}

/* ── Scheduler state ────────────────────────────────────────────────────── */

/* The kernel main thread acts as the idle task (no separate stack). */
static task_t  idle_task;

/* Currently running task on this (single) CPU. */
static task_t *cur_task = (task_t *)0;

/* Set by sched_tick() every timer tick; cleared by sched_irq_end(). */
static volatile int want_schedule    = 0;

/* Guards against sched_irq_end being active before sched_run(). */
static volatile int scheduler_active = 0;

/* boot/multiboot_entry.asm's PML4 physical address — the "shared/master"
 * CR3 every task with task_t.cr3==0 runs under (kernel tasks, WM, smoke
 * tests). Tracked so sched_irq_end only reloads CR3 when it's actually
 * changing (a `mov cr3` flushes the whole TLB — not free). */
#define MASTER_CR3_PHYS 0x1000ULL
static uint64_t loaded_cr3 = MASTER_CR3_PHYS;

/* ── Ring3 stack pool ───────────────────────────────────────────────────── *
 * Lives in .user_bss (linker.ld) so it falls inside the region paging.c
 * marks U=1 — CPL3 code can only ever run on a stack the CPU is allowed to
 * touch at CPL3. Bump-allocated, never freed (mirrors the kernel-stack
 * handling in sched_submit_prio: fine for a handful of long-lived tasks). */
static uint8_t user_stack_pool[MAX_USER_TASKS][USER_STACK_SZ]
    __attribute__((section(".user_bss"), aligned(16)));
static int user_stack_next = 0;

/* ── Forward declaration ────────────────────────────────────────────────── */
static void __attribute__((noreturn)) task_trampoline(void);

/* ── User-task entry trampoline ─────────────────────────────────────────── *
 * task_trampoline (below) lives in kernel .text (U=0) — RIP can't point
 * there for a ring3 task, the very first instruction fetch would #PF. This
 * one lives in .user_text instead, so it's what ring3 tasks actually start
 * executing. It can't read cur_task (kernel-only U=0 memory), so func/arg
 * arrive via rdi/rsi — set directly in the initial iretq frame, matching
 * the SysV calling convention this function expects them in. SYS_EXIT is
 * syscall number 0 (cpu/syscall.h); hardcoded here to avoid a dependency
 * loop (syscall.c will itself run in kernel .text, no need to link .user_text
 * against it — the ABI number is the only shared contract). */
__attribute__((section(".user_text")))
static void user_task_trampoline(void (*func)(void *), void *arg) {
    func(arg);
    __asm__ volatile("int $0x80" :: "a"(0 /* SYS_EXIT */) : "memory");
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}

/* ── frame_build ────────────────────────────────────────────────────────── *
 * Builds an initial register frame at the top of `kstack_top` so that      *
 * irq_common_stub can switch to this task via "mov rsp; pop*; iretq".      *
 * Shared by ring0 (task_init_stack) and ring3 (task_init_stack_user) setup;
 * only rip/cs/ss/target_rsp/rdi/rsi differ between the two.                *
 *                                                                           *
 * The frame mirrors what irq_common_stub pushes/pops:                      *
 *   low addr  r15..rax (15 GPRs) | int_no err_code | rip cs rflags rsp ss */
static uint64_t frame_build(uint64_t kstack_top, uint64_t rip, uint64_t cs,
                            uint64_t ss, uint64_t target_rsp,
                            uint64_t rdi_val, uint64_t rsi_val) {
    uint64_t *sp = (uint64_t *)(uintptr_t)kstack_top;

    /* CPU iretq frame (highest address = first pushed by hardware) */
    *--sp = ss;
    *--sp = target_rsp;
    *--sp = 0x202ULL;   /* rflags: IF=1 */
    *--sp = cs;
    *--sp = rip;

    /* Dummy pushes matching irq0 (push 0; push 32 before irq_common_stub).
     * err_code at higher address, int_no at lower.                         */
    *--sp = 0ULL;   /* err_code */
    *--sp = 0ULL;   /* int_no   */

    /* GPRs in irq_common_stub push order (push rax first → rax at higher
     * address; push r15 last → r15 at lowest address = final RSP).        */
    *--sp = 0ULL;      /* rax */
    *--sp = 0ULL;      /* rbx */
    *--sp = 0ULL;      /* rcx */
    *--sp = 0ULL;      /* rdx */
    *--sp = rsi_val;   /* rsi */
    *--sp = rdi_val;   /* rdi */
    *--sp = 0ULL;      /* rbp */
    *--sp = 0ULL;      /* r8  */
    *--sp = 0ULL;      /* r9  */
    *--sp = 0ULL;      /* r10 */
    *--sp = 0ULL;      /* r11 */
    *--sp = 0ULL;      /* r12 */
    *--sp = 0ULL;      /* r13 */
    *--sp = 0ULL;      /* r14 */
    *--sp = 0ULL;   /* r15  ← returned value points here */

    return (uint64_t)(uintptr_t)sp;
}

/* ── task_trampoline ────────────────────────────────────────────────────── *
 * Entry point for every ring0 task. Entered via iretq with IF=1. (Ring3
 * tasks enter via user_task_trampoline above instead — this one lives in
 * kernel .text, unreachable from CPL3.) */
static void __attribute__((noreturn)) task_trampoline(void) {
    cur_task->func(cur_task->arg);
    sched_exit();
}

/* ── task_init_stack / task_init_stack_user ────────────────────────────── */
static void task_init_stack(task_t *t) {
    uint64_t stack_top = (uint64_t)(uintptr_t)t->stack + TASK_STACK_SZ;
    t->rsp = frame_build(stack_top, (uint64_t)(uintptr_t)task_trampoline,
                         GDT_KERNEL_CS, GDT_KERNEL_DS, stack_top, 0, 0);
}

/* Same idea, but targets ring3: cs/ss carry RPL=3, rsp is the task's own
 * .user_bss stack, and rip is user_task_trampoline (the only kernel-chosen
 * code address ring3 ever starts executing at). func/arg travel in rdi/rsi
 * since the trampoline can't read cur_task from ring3. */
static void task_init_stack_user(task_t *t) {
    uint64_t kstack_top = (uint64_t)(uintptr_t)t->stack + TASK_STACK_SZ;
    uint64_t ustack_top = (uint64_t)(uintptr_t)t->user_stack + USER_STACK_SZ;
    t->rsp = frame_build(kstack_top, (uint64_t)(uintptr_t)user_task_trampoline,
                         GDT_USER_CS | 3, GDT_USER_DS | 3, ustack_top,
                         (uint64_t)(uintptr_t)t->func, (uint64_t)(uintptr_t)t->arg);
}

/* Same as task_init_stack_user, but for a task with its own private address
 * space (cpu/vmm.h): the stack top is caller-supplied (a page the caller
 * already mapped into that address space via vmm_map_page), not derived
 * from the shared .user_bss pool. user_task_trampoline itself stays the
 * entry point — it's part of the kernel's low-memory identity map (PDPT[0]),
 * which every process's PDPT[0] slot references by copy (cpu/vmm.c), so
 * it's mapped and U=1 in every address space, not just the shared one. */
static void task_init_stack_user_as(task_t *t, uint64_t ustack_top) {
    uint64_t kstack_top = (uint64_t)(uintptr_t)t->stack + TASK_STACK_SZ;
    t->rsp = frame_build(kstack_top, (uint64_t)(uintptr_t)user_task_trampoline,
                         GDT_USER_CS | 3, GDT_USER_DS | 3, ustack_top,
                         (uint64_t)(uintptr_t)t->func, (uint64_t)(uintptr_t)t->arg);
}

/* ── sched_init ─────────────────────────────────────────────────────────── */
void sched_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_pool[i].state = TASK_FREE;
        task_pool[i].id    = -1;
    }

    idle_task.id        = -1;
    idle_task.state     = TASK_RUNNING;
    idle_task.priority  = PRIO_IDLE_LVL;
    idle_task.stack     = (uint8_t *)0;
    idle_task.timeslice = 0;
    idle_task.quantum   = 0;
    idle_task.func      = (void *)0;
    idle_task.arg       = (void *)0;
    idle_task.cpu_id    = 0;
    idle_task.name[0]   = 'i'; idle_task.name[1] = 'd';
    idle_task.name[2]   = 'l'; idle_task.name[3] = 'e';
    idle_task.name[4]   = '\0';
}

/* ── sched_run ──────────────────────────────────────────────────────────── *
 * Enable preemptive scheduling. The calling thread becomes the idle task.  *
 * Returns immediately; caller continues running as the idle context.       */
void sched_run(void) {
    cur_task         = &idle_task;
    scheduler_active = 1;
}

/* Finds a reusable task_pool slot (FREE or DONE), growing task_count if the
 * pool hasn't filled up yet. Caller must already hold IF=0. -1 if full. */
static int find_free_slot(void) {
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].state == TASK_FREE || task_pool[i].state == TASK_DONE)
            return i;
    }
    if (task_count >= MAX_TASKS) return -1;
    return task_count++;
}

static void task_set_name(task_t *t, const char *name) {
    int ni = 0;
    while (ni < 23 && name[ni]) { t->name[ni] = name[ni]; ni++; }
    t->name[ni] = '\0';
}

/* ── sched_submit_prio ──────────────────────────────────────────────────── */
int sched_submit_prio(const char *name, void (*func)(void *), void *arg,
                      uint8_t priority) {
    if (!func || priority >= PRIO_LEVELS) return -1;

    /* Disable interrupts to prevent timer from running sched_irq_end while
     * we are modifying the task pool and run queue.                        */
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags));

    int slot = find_free_slot();
    if (slot < 0) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
        return -1;
    }

    /* Allocate task stack */
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SZ, 16, (uintptr_t *)0);
    if (!stack) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
        return -1;
    }

    task_t *t   = &task_pool[slot];
    t->id       = slot;
    t->state    = TASK_READY;
    t->priority = priority;
    t->cpu_id   = -1;
    t->stack    = stack;
    t->is_user  = 0;
    t->user_stack = (uint8_t *)0;
    t->cr3      = 0;   /* reset in case this slot's previous occupant was a private-AS task */
    t->quantum  = prio_quantum[priority];
    t->timeslice= t->quantum;
    t->func     = func;
    t->arg      = arg;

    task_set_name(t, name);
    task_init_stack(t);
    rq_push(priority, t);

    __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
    return slot;
}

int sched_submit(const char *name, void (*func)(void *), void *arg) {
    return sched_submit_prio(name, func, arg, PRIO_NORMAL);
}

/* ── sched_submit_user ─────────────────────────────────────────────────── */
int sched_submit_user(const char *name, void (*entry)(void *), void *arg) {
    if (!entry) return -1;

    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags));

    if (user_stack_next >= MAX_USER_TASKS) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
        return -1;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
        return -1;
    }

    /* Ring0 trap stack — used only while this task is handling a syscall or
     * exception; TSS.RSP0 points here whenever it's running (sched_irq_end). */
    uint8_t *kstack = (uint8_t *)kmalloc(TASK_STACK_SZ, 16, (uintptr_t *)0);
    if (!kstack) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
        return -1;
    }

    task_t *t     = &task_pool[slot];
    t->id         = slot;
    t->state      = TASK_READY;
    t->priority   = PRIO_NORMAL;
    t->cpu_id     = -1;
    t->stack      = kstack;
    t->user_stack = user_stack_pool[user_stack_next++];
    t->is_user    = 1;
    t->cr3        = 0;   /* shared/master address space */
    t->quantum    = prio_quantum[PRIO_NORMAL];
    t->timeslice  = t->quantum;
    t->func       = entry;
    t->arg        = arg;

    task_set_name(t, name);
    task_init_stack_user(t);
    rq_push(PRIO_NORMAL, t);

    __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
    return slot;
}

/* ── sched_submit_user_as ──────────────────────────────────────────────── */
int sched_submit_user_as(const char *name, void (*entry)(void *), void *arg,
                         uint64_t cr3, uint64_t user_stack_top) {
    if (!entry || !cr3) return -1;

    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags));

    int slot = find_free_slot();
    if (slot < 0) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
        return -1;
    }

    /* Ring0 trap stack, same role as in sched_submit_user — the process's
     * own address space has no kernel-mode stack of its own to trap into. */
    uint8_t *kstack = (uint8_t *)kmalloc(TASK_STACK_SZ, 16, (uintptr_t *)0);
    if (!kstack) {
        __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
        return -1;
    }

    task_t *t     = &task_pool[slot];
    t->id         = slot;
    t->state      = TASK_READY;
    t->priority   = PRIO_NORMAL;
    t->cpu_id     = -1;
    t->stack      = kstack;
    t->user_stack = (uint8_t *)0;   /* not from the shared pool — private AS instead */
    t->is_user    = 1;
    t->cr3        = cr3;
    t->quantum    = prio_quantum[PRIO_NORMAL];
    t->timeslice  = t->quantum;
    t->func       = entry;
    t->arg        = arg;

    task_set_name(t, name);
    task_init_stack_user_as(t, user_stack_top);
    rq_push(PRIO_NORMAL, t);

    __asm__ volatile("pushq %0; popfq" :: "r"(rflags));
    return slot;
}

/* ── sched_tick ─────────────────────────────────────────────────────────── *
 * Called from timer_callback every millisecond. Signals a reschedule.      */
void sched_tick(void) {
    if (scheduler_active)
        want_schedule = 1;
}

/* ── sched_irq_end ──────────────────────────────────────────────────────── *
 * Called from irq_common_stub (in interrupt.asm) after irq_handler.        *
 * cur_rsp = RSP after all GPR pushes (= pointer to saved register frame).  *
 * Returns new RSP: same as cur_rsp (no switch) or new task's saved RSP.   */
uint64_t sched_irq_end(uint64_t cur_rsp) {
    if (!scheduler_active || !want_schedule)
        return cur_rsp;

    want_schedule = 0;
    cur_task->rsp = cur_rsp;   /* save current task's context */

    /* Re-queue the current task unless it's idle, done, or blocked */
    if (cur_task != &idle_task && cur_task->state == TASK_RUNNING) {
        if (--cur_task->timeslice > 0)
            return cur_rsp;   /* quantum not expired, keep running */
        cur_task->timeslice = cur_task->quantum;
        cur_task->state     = TASK_READY;
        rq_push(cur_task->priority, cur_task);
    }

    /* Pick highest-priority ready task; fall back to idle if queues empty */
    task_t *next = (task_t *)0;
    for (int p = 0; p < PRIO_LEVELS && !next; p++) {
        if (rq_size[p] > 0)
            next = rq_pop(p);
    }
    if (!next) next = &idle_task;

    if (next == cur_task) {
        /* We're the only ready task: got re-queued and immediately re-popped
         * as `next` above, but that left state==TASK_READY (set at line
         * ~317) even though we're not actually switching away — restore it,
         * or the *next* sched_irq_end call sees state != TASK_RUNNING, skips
         * re-queueing us (line 313's guard), and we silently fall out of
         * every run queue for good (picked up by no one, ever rescheduled). */
        cur_task->state = TASK_RUNNING;
        return cur_rsp;
    }

    /* Ring3 tasks trap back into ring0 on their own kstack (TSS.RSP0) — point
     * it at this task's before letting it run, or the next trap/syscall from
     * it would land on whatever task last set RSP0. */
    if (next->is_user)
        tss_set_rsp0((uint64_t)(uintptr_t)next->stack + TASK_STACK_SZ);

    /* Load next's address space if it differs from what's currently active
     * — covers switching into a private-AS process (cpu/vmm.h) and back out
     * to the shared/master one (task_t.cr3==0), symmetrically. */
    {
        uint64_t want_cr3 = next->cr3 ? next->cr3 : MASTER_CR3_PHYS;
        if (want_cr3 != loaded_cr3) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(want_cr3) : "memory");
            loaded_cr3 = want_cr3;
        }
    }

    cur_task        = next;
    cur_task->state = TASK_RUNNING;
    return cur_task->rsp;   /* switch stacks */
}

/* ── sched_kill_current ────────────────────────────────────────────────── *
 * Called from isr_handler when a ring3 task faults. Marks it DONE so the
 * sched_irq_end call that follows (isr_common_stub, mirroring irq_common_stub)
 * switches away from it instead of resuming a corrupted context. */
void sched_kill_current(void) {
    if (cur_task && cur_task != &idle_task) {
        cur_task->state = TASK_DONE;
        want_schedule    = 1;
    }
}

/* Marks the current task's timeslice expired so the next sched_irq_end call
 * re-queues it and switches away. Shared by sched_yield (below, for callers
 * running normal kernel code) and SYS_YIELD (cpu/syscall.c) — the syscall
 * path is already inside syscall_stub's own interrupt frame, whose epilogue
 * runs sched_irq_end right after syscall_dispatch returns, so it must NOT
 * also do `int $0x20` — nesting a second interrupt frame on top of that one
 * just to request the same thing the outer frame is about to do anyway. */
void sched_want_reschedule(void) {
    if (cur_task != &idle_task && cur_task->state == TASK_RUNNING)
        cur_task->timeslice = 1;
    want_schedule = 1;
}

/* ── sched_yield ────────────────────────────────────────────────────────── */
void sched_yield(void) {
    sched_want_reschedule();
    __asm__ volatile("int $0x20");
}

/* ── sched_exit ─────────────────────────────────────────────────────────── */
void __attribute__((noreturn)) sched_exit(void) {
    cur_task->state = TASK_DONE;
    want_schedule   = 1;
    __asm__ volatile("int $0x20");
    for (;;) __asm__ volatile("hlt");
}

/* ── sched_run_loop (APs) ───────────────────────────────────────────────── *
 * APs halt here; the timer's sched_irq_end will eventually schedule work.  */
void sched_run_loop(int cpu_id) {
    (void)cpu_id;
    for (;;) __asm__ volatile("hlt");
}

/* ── Accessors ──────────────────────────────────────────────────────────── */
int     sched_task_count(void)    { return task_count; }
task_t *sched_get_task(int idx) {
    if (idx < 0 || idx >= task_count) return (task_t *)0;
    return &task_pool[idx];
}

const char *sched_state_name(task_state_t s) {
    switch (s) {
    case TASK_FREE:    return "free";
    case TASK_READY:   return "ready";
    case TASK_RUNNING: return "running";
    case TASK_BLOCKED: return "blocked";
    case TASK_DONE:    return "done";
    default:           return "?";
    }
}
