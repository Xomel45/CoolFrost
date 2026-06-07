#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* Local APIC MMIO base — mapped in page tables at boot */
#define LAPIC_BASE        ((volatile uint32_t *)0xFEE00000UL)

/* Register offsets (byte offsets, divide by 4 for uint32_t index) */
#define LAPIC_ID          0x020
#define LAPIC_VER         0x030
#define LAPIC_TPR         0x080   /* Task Priority */
#define LAPIC_EOI         0x0B0   /* End Of Interrupt (write 0) */
#define LAPIC_SVR         0x0F0   /* Spurious Interrupt Vector */
#define LAPIC_ICR_LO      0x300   /* Interrupt Command low  */
#define LAPIC_ICR_HI      0x310   /* Interrupt Command high */
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_TIMER_ICR   0x380   /* Initial Count */
#define LAPIC_TIMER_CCR   0x390   /* Current Count (read-only) */
#define LAPIC_TIMER_DCR   0x3E0   /* Divide Config */

/* ICR delivery modes */
#define ICR_FIXED         0x00000000
#define ICR_INIT          0x00000500
#define ICR_SIPI          0x00000600

/* ICR shorthand */
#define ICR_NO_SHORTHAND  0x00000000
#define ICR_ALL_EXCL_SELF 0x000C0000

/* ICR level / trigger */
#define ICR_ASSERT        0x00004000
#define ICR_DEASSERT      0x00000000
#define ICR_LEVEL_TRIG    0x00008000
#define ICR_EDGE_TRIG     0x00000000

/* Read / write a LAPIC register */
static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)((uintptr_t)0xFEE00000UL + reg);
}
static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)((uintptr_t)0xFEE00000UL + reg) = val;
}

/* Returns the local APIC ID of the current CPU (bits [31:24] of LAPIC_ID) */
static inline uint8_t lapic_id(void) {
    return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

void lapic_enable(void);
void lapic_eoi(void);
void lapic_wait_icr_idle(void);

/* IPI helpers */
void lapic_send_init(uint8_t apic_id);
void lapic_send_sipi(uint8_t apic_id, uint8_t vector);

#endif
