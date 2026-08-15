#include "usyscall.h"

/* Deliberately calls SYS_READ with a pointer that's inside the process's
 * 1GB VMM window (0x40000000, must match cpu/vmm.h: VMM_USER_BASE — not
 * included here, userland headers never share kernel ones, see
 * usyscall.h) but nowhere near anything actually mapped: not the loaded
 * segments (which start at 0x40001000, tiny), not the stack, not the argv
 * page (both pinned near the window's ceiling). Before cpu/paging.c's
 * is_user_range() started walking the process's own page tables
 * (vmm_range_mapped, cpu/vmm.c), this pointer would have passed the plain
 * bounds check, and the kernel would have #PF'd in ring0 trying to read
 * through it — a fault cpu/isr.c's handler doesn't recover from, hanging
 * the whole kernel, not just this task. Confirms the fix: the syscall must
 * come back rejected (-1), and this process must exit cleanly afterward. */

static const char ok_msg[]  = "badptr: syscall correctly rejected the unmapped pointer\n";
static const char bad_msg[] = "badptr: syscall did NOT reject it (should be unreachable)\n";

void _start(void *arg) {
    (void)arg;

    char *bad_ptr = (char *)(0x40000000ULL + 0x10000000ULL);
    uint64_t ret = usyscall3(SYS_READ, 0 /* fd — never used, rejected before that */,
                             (uint64_t)(unsigned long)bad_ptr, 16);

    if (ret == (uint64_t)-1)
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)ok_msg, sizeof(ok_msg) - 1);
    else
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)bad_msg, sizeof(bad_msg) - 1);

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
