#include "powerctl.h"
#include "../cpu/ports.h"

void halt() {
loop:
    asm volatile("hlt");
    goto loop;
}

// THE CODE TOOK FROM OSDEV WIKI: https://wiki.osdev.org/Reboot#Annotated_code_for_reboot

/* keyboard interface IO port: data and control
   READ:   status port
   WRITE:  control register */
#define KBRD_INTRFC 0x64

/* keyboard interface bits */
#define KBRD_BIT_KDATA 0 /* keyboard data is in buffer (output buffer is empty) (bit 0) */
#define KBRD_BIT_UDATA 1 /* user data is in buffer (command buffer is empty) (bit 1) */

#define KBRD_IO 0x60 /* keyboard IO port */
#define KBRD_RESET 0xFE /* reset CPU command */

#define bit(n) (1<<(n)) /* Set bit n to 1 */

/* Check if bit n in flags is set */
#define check_flag(flags, n) ((flags) & bit(n))

void reboot()
{
    uint8_t temp;

    asm volatile ("cli"); /* disable all interrupts */

    /* Clear all keyboard buffers (output and command buffers) */
    do
    {
        temp = port_byte_in(KBRD_INTRFC); /* empty user data */
        if (check_flag(temp, KBRD_BIT_KDATA) != 0)
            port_byte_in(KBRD_IO); /* empty keyboard data */
    } while (check_flag(temp, KBRD_BIT_UDATA) != 0);

    port_byte_out(KBRD_INTRFC, KBRD_RESET); /* pulse CPU reset line */
loop:
    asm volatile ("hlt"); /* if that didn't work, halt the CPU */
    goto loop; /* if a NMI is received, halt again */
}

// -----------------------------------------------------------------------------

/*
 * poweroff() — ACPI S5 shutdown.
 *
 * Tries several well-known ACPI PM1a control port addresses used by QEMU
 * and Bochs/VirtualBox.  Falls back to CLI + HLT loop if none respond.
 *
 * PM1a_CNT write: bits [12:10] = SLP_TYP (5 for S5), bit [13] = SLP_EN
 * Value: (5 << 10) | (1 << 13) = 0x3400
 * QEMU PIIX4 PM1a_CNT port = 0x0004 (PMBASE defaults to 0x0600 → 0x0604)
 * QEMU Q35   PM1a_CNT port = 0x4004
 * Bochs      PM1a_CNT port = 0xB004 (value 0x2000 — different S5 type)
 */
void poweroff(void) {
    asm volatile("cli");

    /* QEMU PIIX4 / SeaBIOS */
    port_word_out(0x0604, 0x2000);
    /* QEMU Q35 chipset */
    port_word_out(0x4004, 0x3400);
    /* Bochs / older VirtualBox */
    port_word_out(0xB004, 0x2000);

    /* If all ACPI attempts fail, halt */
    for (;;) asm volatile("hlt");
}
