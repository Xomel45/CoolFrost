#include "smp.h"
#include "apic.h"
#include "idt.h"
#include "sched.h"
#include "../cpu/timer.h"
#include <stdint.h>

/* ── AP mailbox at 0x7000 ───────────────────────────────────────────────── */
#define MAILBOX_BASE      0x7000UL
#define MB_AP_MAIN        0x00    /* uint64_t: ap_main address              */
#define MB_AP_RSP         0x08    /* uint64_t: AP stack pointer             */
#define MB_AP_CR3         0x10    /* uint32_t: BSP's CR3                    */
#define MB_AP_READY       0x14    /* uint32_t: AP writes 1 when in 64-bit   */
#define MB_AP_INDEX       0x18    /* uint32_t: which AP index (0-based)     */

#define AP_TRAMPOLINE_PHYS  0x8000UL   /* must match SIPI vector × 4096     */
#define AP_SIPI_VECTOR      0x08       /* 0x08 × 0x1000 = 0x8000            */

/* Trampoline blob exported by ap_trampoline_embed.asm */
extern uint8_t  ap_trampoline_blob[];
extern uint64_t ap_trampoline_size;

/* Per-AP stacks — in BSS, identity-mapped, 8 KB each */
static uint8_t ap_stacks[MAX_CPUS][8192] __attribute__((aligned(16)));

/* Global: number of online CPUs (BSP = 1; APs increment as they start) */
volatile int smp_cpu_count = 1;

/* BSP's GDT descriptor, for APs to reload after loading the BSP's GDT.
 * Declared in boot/gdt.asm with `global gdt64_descriptor`. */
extern uint8_t gdt64_descriptor[];

/* ── Mailbox helpers ────────────────────────────────────────────────────── */
static inline void mb_write64(uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(MAILBOX_BASE + off) = v;
}
static inline void mb_write32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(MAILBOX_BASE + off) = v;
}
static inline uint32_t mb_read32(uint32_t off) {
    return *(volatile uint32_t *)(MAILBOX_BASE + off);
}

/* ── smp_init ────────────────────────────────────────────────────────────── */
void smp_init(void) {
    /* 1. Copy the trampoline to physical 0x8000 */
    uint8_t *dst = (uint8_t *)AP_TRAMPOLINE_PHYS;
    for (uint64_t i = 0; i < ap_trampoline_size; i++)
        dst[i] = ap_trampoline_blob[i];

    /* 2. Read BSP's CR3 */
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    /* 3. Enable BSP Local APIC */
    lapic_enable();

    /* 4. Fill constant mailbox fields */
    mb_write64(MB_AP_MAIN, (uint64_t)(uintptr_t)ap_main);
    mb_write32(MB_AP_CR3,  (uint32_t)cr3);

    /* 5. Try to start APs sequentially (APIC IDs 1, 2, …) */
    for (int ap_idx = 0; ap_idx < MAX_CPUS - 1; ap_idx++) {
        uint8_t apic_id = (uint8_t)(ap_idx + 1); /* BSP is APIC 0 */

        /* Set per-AP stack (top of the stack region, 16-byte aligned) */
        uint8_t *sp = ap_stacks[ap_idx + 1] + sizeof(ap_stacks[0]) - 16;
        mb_write64(MB_AP_RSP,   (uint64_t)(uintptr_t)sp);
        mb_write32(MB_AP_INDEX, (uint32_t)ap_idx);
        mb_write32(MB_AP_READY, 0);

        /* Send INIT IPI */
        lapic_send_init(apic_id);

        /* Send SIPI */
        lapic_send_sipi(apic_id, AP_SIPI_VECTOR);

        /* Wait up to 50 ms for the AP to signal ready */
        int ok = 0;
        for (int t = 0; t < 50; t++) {
            sleep_ms(1);
            if (mb_read32(MB_AP_READY) == 1) { ok = 1; break; }
        }

        if (!ok) break;  /* No more APs at this APIC ID — stop */

        smp_cpu_count++;
    }
}

/* ── ap_main — runs on each AP after the trampoline ────────────────────── */
void ap_main(int ap_idx) {
    /* Reload BSP's GDT (trampoline left us with CS=0x18 which still works
     * because we added a duplicate code64 entry at 0x18 in the BSP's GDT). */
    asm volatile("lgdt (%0)" :: "r"(gdt64_descriptor) : "memory");

    /* Load the BSP's IDT (already fully set up by isr_install) */
    asm volatile("lidt (%0)" :: "r"(&idt_reg) : "memory");

    /* Enable this CPU's Local APIC */
    lapic_enable();

    /* Enable interrupts */
    asm volatile("sti");

    /* Enter the task scheduler loop — never returns */
    sched_run_loop(ap_idx + 1);   /* cpu_id: AP 0 → CPU 1, etc. */
}
