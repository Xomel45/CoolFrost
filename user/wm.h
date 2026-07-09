#ifndef USER_WM_H
#define USER_WM_H

/* Entry point for the ring3 WM task (sched_submit_user target — see the
 * `wm` shell command in kernel/kernel.c). Runs the compositor loop until
 * ESC, then returns (user_task_trampoline follows up with SYS_EXIT). */
void wm_run(void *arg);

#endif
