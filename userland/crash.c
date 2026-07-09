#include "usyscall.h"

/* Deliberately writes through a kernel-only pointer — verifies that a
 * page-table-isolated process (its own CR3, cpu/vmm.h) gets killed by
 * isr_handler/sched_kill_current exactly like the shared-arena ring3 tasks
 * (user/smoke.c) do, without taking the kernel down. */
void _start(void *arg) {
    (void)arg;
    volatile int *bad = (volatile int *)0x2000;   /* PDPT page, always U=0 */
    *bad = 1;
    usyscall0(SYS_EXIT);   /* unreachable if isolation actually works */
}
