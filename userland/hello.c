#include "usyscall.h"

static const char msg[] = "hello from ring3 process (private address space)\n";

void _start(void *arg) {
    (void)arg;
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)msg, sizeof(msg) - 1);
    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
