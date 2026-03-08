#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Segment selectors */
#define KERNEL_CS 0x08

/*
 * 64-bit IDT gate descriptor (16 bytes per entry).
 * Ref: Intel SDM Vol.3 §6.14.1
 */
typedef struct {
    uint16_t low_offset;    /* bits  0-15  of handler address */
    uint16_t sel;           /* kernel code segment selector   */
    uint8_t  ist;           /* interrupt stack table index (0 = none) */
    uint8_t  flags;         /* P, DPL, type (0x8E = 64-bit interrupt gate) */
    uint16_t mid_offset;    /* bits 16-31  of handler address */
    uint32_t high_offset;   /* bits 32-63  of handler address */
    uint32_t zero;          /* reserved, must be 0 */
} __attribute__((packed)) idt_gate_t;

/* IDTR: limit is 16-bit, base is 64-bit in long mode */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_register_t;

#define IDT_ENTRIES 256
extern idt_gate_t    idt[IDT_ENTRIES];
extern idt_register_t idt_reg;

void set_idt_gate(int n, uint64_t handler);
void set_idt(void);

#endif
