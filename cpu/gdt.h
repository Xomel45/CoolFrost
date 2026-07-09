#ifndef GDT_H
#define GDT_H

/* Selectors in the permanent, C-built GDT installed by gdt_init().
 * 0x08/0x10/0x18 are byte-identical to boot/gdt.asm's early GDT so nothing
 * that already hardcodes them (idt.c's KERNEL_CS, interrupt.asm's `mov ax,
 * 0x10`, sched.c's task frame) needs to change. APs still LGDT the original
 * asm table via the trampoline — that's fine, they only ever use these same
 * three kernel selectors and never touch the user/TSS entries below. */
#define GDT_KERNEL_CS    0x08
#define GDT_KERNEL_DS    0x10
#define GDT_KERNEL_CS_AP 0x18
#define GDT_USER_CS      0x20   /* DPL=3 */
#define GDT_USER_DS      0x28   /* DPL=3 */
#define GDT_TSS_SEL      0x30

/* Builds the permanent GDT (kernel + user + TSS descriptors), loads it,
 * reloads all segment registers, and installs the TSS. Must run with
 * interrupts disabled, before irq_install()/sched_run(). */
void gdt_init(void);

#endif
