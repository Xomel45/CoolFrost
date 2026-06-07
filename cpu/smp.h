#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define MAX_CPUS 8

/* Number of CPUs currently online (BSP = 1, APs increment this). */
extern volatile int smp_cpu_count;

/*
 * Initialise SMP: copy trampoline, send INIT+SIPI to each AP found.
 * Must be called after LAPIC is accessible (page tables set up) and
 * after the IDT is installed (BSP has set idt_reg).
 */
void smp_init(void);

/* Entry point for Application Processors (called from trampoline). */
void ap_main(int ap_idx);

#endif
