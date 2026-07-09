#ifndef TSS_H
#define TSS_H

#include <stdint.h>

/* 64-bit Task State Segment (Intel SDM Vol.3 §8.7). We only use RSP0 (the
 * stack the CPU loads on a ring3→ring0 transition) and leave IST/RSP1-2
 * unused. iopb_offset is set past the segment limit so no I/O bitmap is
 * present — ring3 code can never execute IN/OUT regardless of EFLAGS.IOPL. */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

/* Writes the TSS descriptor into gdt[6]/gdt[7] and loads TR via LTR.
 * Must run after the GDT carrying those slots is already active (LGDT'd). */
void tss_install(uint64_t *gdt);

/* Updates the ring0 stack the CPU will use on the next ring3→ring0 trap.
 * Called by the scheduler right before switching onto a user task. */
void tss_set_rsp0(uint64_t rsp0);

#endif
