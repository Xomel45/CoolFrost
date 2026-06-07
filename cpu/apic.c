#include "apic.h"
#include "../cpu/timer.h"

void lapic_enable(void) {
    /* Accept all interrupts (TPR = 0) */
    lapic_write(LAPIC_TPR, 0);
    /* Enable LAPIC, spurious vector = 0xFF */
    lapic_write(LAPIC_SVR, (1u << 8) | 0xFF);
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

void lapic_wait_icr_idle(void) {
    /* Bit 12 of ICR_LO = delivery status (1 = send pending) */
    while (lapic_read(LAPIC_ICR_LO) & (1u << 12))
        asm volatile("pause");
}

/*
 * Send INIT IPI to a specific AP (assert then de-assert).
 * apic_id: the target Local APIC ID.
 */
void lapic_send_init(uint8_t apic_id) {
    /* Assert INIT */
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, ICR_INIT | ICR_ASSERT | ICR_LEVEL_TRIG);
    lapic_wait_icr_idle();
    sleep_ms(10);

    /* De-assert INIT */
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, ICR_INIT | ICR_DEASSERT | ICR_LEVEL_TRIG);
    lapic_wait_icr_idle();
    sleep_ms(10);
}

/*
 * Send SIPI (Startup IPI) to a specific AP.
 * vector: trampoline page index (physical address = vector * 4096).
 * For trampoline at 0x8000, vector = 8.
 */
void lapic_send_sipi(uint8_t apic_id, uint8_t vector) {
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, ICR_SIPI | ICR_ASSERT | ICR_EDGE_TRIG | vector);
    lapic_wait_icr_idle();
}
