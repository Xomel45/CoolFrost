#include "gdt.h"
#include "tss.h"
#include <stdint.h>

#define GDT_ENTRIES 8

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_register_t;

static uint64_t      gdt[GDT_ENTRIES];
static gdt_register_t gdt_reg;

/* LGDT the new table, reload DS/ES/FS/GS/SS to kernel data, then reload CS
 * via a far return (push target CS:RIP, lretq) — there is no `mov` for CS,
 * and even though 0x08 is byte-identical to the table we just replaced, the
 * CPU's cached descriptor state must be refreshed against the new table. */
static void gdt_flush(uint64_t gdt_reg_addr) {
    __asm__ volatile(
        "lgdt (%0)\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq $0x08\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "r"(gdt_reg_addr)
        : "rax", "memory"
    );
}

void gdt_init(void) {
    gdt[0] = 0x0000000000000000ULL;   /* null */
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code, DPL0, L=1 */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data, DPL0 */
    gdt[3] = 0x00AF9A000000FFFFULL;   /* 0x18 kernel code, AP-trampoline compat */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* 0x20 user code,   DPL3, L=1 */
    gdt[5] = 0x00CFF2000000FFFFULL;   /* 0x28 user data,   DPL3 */
    /* gdt[6..7]: TSS descriptor, filled in by tss_install() */

    gdt_reg.limit = sizeof(gdt) - 1;
    gdt_reg.base  = (uint64_t)&gdt;

    gdt_flush((uint64_t)&gdt_reg);
    tss_install(gdt);
}
