#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

#define MAX_TASKS 32

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_DONE    = 2,
} task_state_t;

typedef struct {
    int          id;
    char         name[24];
    task_state_t state;
    void       (*func)(void *arg);
    void        *arg;
    int          cpu_id;   /* which CPU executed this (-1 = not yet) */
} task_t;

/*
 * Submit a task to the global queue.
 * Returns the task id, or -1 if the queue is full.
 */
int sched_submit(const char *name, void (*func)(void *), void *arg);

/*
 * The AP/BSP scheduler loop: picks ready tasks and runs them.
 * cpu_id identifies which CPU is calling (0 = BSP, 1+ = APs).
 * Returns only when no more tasks will ever appear (use for APs only).
 */
void sched_run_loop(int cpu_id);

/* Accessors for the shell ps command */
int     sched_task_count(void);
task_t *sched_get_task(int idx);

#endif
