#ifndef USER_SMOKE_H
#define USER_SMOKE_H

/* A5.5 smoke test (see plan): two ring3 tasks that exercise the GDT/TSS/
 * iretq-frame/RSP0 plumbing before anything WM-shaped is built on top. */

/* Runs harmlessly in ring3 and exits cleanly via SYS_EXIT. */
void user_smoke_ok(void *arg);

/* Deliberately writes through a kernel-only (U=0) pointer — expected to
 * #PF and get killed by isr_handler/sched_kill_current instead of hanging
 * or taking the rest of the kernel down with it. */
void user_smoke_fault(void *arg);

#endif
