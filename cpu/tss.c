#include "tss.h"
#include "../libc/mem.h"

#define TSS_SEL 0x30

static tss_t tss;

void tss_install(uint64_t *gdt) {
    memset(&tss, 0, sizeof(tss));
    tss.iopb_offset = sizeof(tss_t);   /* no I/O bitmap -> ring3 IN/OUT faults */

    uint64_t base  = (uint64_t)&tss;
    uint32_t limit = sizeof(tss_t) - 1;

    /* 64-bit TSS descriptor: 16 bytes / two GDT slots (SDM Vol.3 §7.2.3) */
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= (uint64_t)0x89 << 40;                    /* P=1, DPL=0, type=0x9 (avail 64-bit TSS) */
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;    /* G=0, AVL=0, limit[19:16] */
    low |= ((base >> 24) & 0xFFULL) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFFULL;

    gdt[6] = low;
    gdt[7] = high;

    __asm__ volatile("ltr %%ax" :: "a"((uint16_t)TSS_SEL));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
