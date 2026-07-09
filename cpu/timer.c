#include "timer.h"
#include "isr.h"
#include "ports.h"
#include "sched.h"
#include "../libc/function.h"
#include "../drivers/usbhid.h"

uint64_t tick = 0;
static uint64_t pit_freq;

static void timer_callback(registers_t *regs) {
    tick++;
    sched_tick();
    usbhid_poll();
    UNUSED(regs);
}

uint64_t get_tick() {
    return tick;
}

void init_timer(uint32_t freq) {
    /* Install the function we just wrote */
    register_interrupt_handler(IRQ0, timer_callback);
    // freq = How fast to callback once in ~1 us
    pit_freq = freq;

    /* Get the PIT value: hardware clock at 1193180 Hz */
    uint32_t divisor = 1193180 / freq;
    uint8_t low  = (uint8_t)(divisor & 0xFF);
    uint8_t high = (uint8_t)( (divisor >> 8) & 0xFF);
    /* Send the command */
    port_byte_out(0x43, 0x36); /* Command port */
    port_byte_out(0x40, low);
    port_byte_out(0x40, high);
}

void sleep_ms(uint64_t time_ms) {
    /* Convert requested time to ticks */
    uint64_t ticks_needed = time_ms;

    if (ticks_needed == 0) ticks_needed = 1; // at least 1 tick

    uint64_t start_tick = tick;

    while ((tick - start_tick) < ticks_needed) {
        /* busy-wait */
        asm volatile("hlt"); // halt CPU until next interrupt to reduce power
    }
}
