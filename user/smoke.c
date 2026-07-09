#include "smoke.h"
#include "usyscall.h"

__attribute__((section(".user_text")))
void user_smoke_ok(void *arg) {
    (void)arg;
    volatile int x = 0;
    for (int i = 0; i < 1000; i++) x += i;
    usyscall0(SYS_EXIT);
}

__attribute__((section(".user_text")))
void user_smoke_fault(void *arg) {
    (void)arg;
    /* 0x2000 is the PDPT page (boot/multiboot_entry.asm) — always mapped,
     * always U=0. A ring3 write here must #PF, not succeed. */
    volatile int *bad = (volatile int *)0x2000;
    *bad = 1;
    usyscall0(SYS_EXIT);   /* unreachable if isolation actually works */
}
