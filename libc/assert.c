#include "assert.h"
#include "stdio.h"

__attribute__((noreturn))
void __kpanic(const char *file, int line, const char *msg) {
    __asm__ volatile("cli");
    printf("\n\n[KERNEL PANIC] %s\n  at %s:%d\n\nSystem halted.\n", msg, file, line);
    for (;;) __asm__ volatile("hlt");
}
