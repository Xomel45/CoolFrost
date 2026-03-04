#include "../cpu/isr.h"
#include "../drivers/screen.h"
#include "../drivers/clock.h"
#include "../libc/stdio.h"
#include "../libc/mem.h"
#include "../libc/stdlib.h"
#include "../cpu/timer.h"
#include "../drivers/keyboard.h"
#include "../libc/heap.h"
#include "../drivers/pcspk.h"
#include "../drivers/cpuid.h"
#include "globals.h"
#include "multiboot.h"
#include "../power/powerctl.h"
#include "../power/acpi.h"

#include <stdint.h>

char *buffer;
static kernel_globals globals;

void perform_tests() {
    kprint_attr("Test 1:\n", GREEN_FG);
    kprint("Getting time:\n");
    datetime_t d = globals.datetime;
    printf("Time: %u:%u:%u Date: %u.%u.%u\n", d.hours, d.minutes, d.seconds, d.day, d.month, d.year);

    kprint_attr("Test 2:\n", GREEN_FG);
    kprint("Fetching memory amonut...\n");
    printf("Lower memory: %ld KB\nUpper memory: %ld KB\n",
            mb_get_lower_mem(globals.multiboot_addr)/1024,
            mb_get_upper_mem(globals.multiboot_addr)/1024);

    kprint_attr("Test 3:\n", GREEN_FG);
    kprint("Fetching CPU info:\n");
    if (globals.cpuid_supported) {
        printf("Manufacturer: %s\nFull name: %s\n",
                globals.cpu_manufacturer,
                globals.cpu_full_name);
    } else {
        kprint("No CPUID detected.\b");
    }
    /*while (1) {
        asm volatile("hlt");
    }*/
}

kernel_globals *get_kernel_globals() {
    return &globals;
}

// Function that is called at the very start of the kernel to perform needed operations before, like initializing everything
void kstart(uint32_t addr) {
    isr_install();
    irq_install();

    // Initializing globals
    globals.multiboot_addr = (multiboot_info_t *)addr;
    rtc_read_datetime(&globals.datetime);
    // If CPUID detected
    if (check_cpuid()) {
        globals.cpuid_supported = 1;
        get_cpu_manufacturer(globals.cpu_manufacturer);
        get_cpu_human_id(globals.cpu_full_name);
    } else {
        globals.cpuid_supported = 0;
        memset((uint8_t *)globals.cpu_manufacturer, 0, 13);
        memset((uint8_t *)globals.cpu_full_name, 0, 49);
    }
    globals.kernel_name = "FrostOS";
    globals.kernel_version = "0.2A";
    globals.kernel_codename = "Snowy House";

    perform_tests();
}

void kernel_main(uint32_t magic, uint32_t addr) {
    kstart(addr);
    sleep_ms(5000);
    clear_screen();

    KHEAPBM kheap;
    k_heapBMInit(&kheap);
    k_heapBMAddBlock(&kheap, (uint32_t *)0x800000, 0x100000, 16);
    buffer = (char *)k_heapBMAlloc(&kheap, 512);
    printf("%s %s\n",
            globals.kernel_name,
            globals.kernel_version);

    printf("RSDP/XSDP Revision: %d\n", validate_rsdp(find_rsdp()));

    while (1) {
        kprint("\n> ");
        memset((uint8_t *)buffer, 0, 512);
        getline(buffer, 1, 512);
        kprint("\n");

        if (strcmp(buffer, "time") == 0) {
            printf("%u:%u:%u\n",
                    globals.datetime.hours,
                    globals.datetime.minutes,
                    globals.datetime.seconds);
        } else if (strcmp(buffer, "date") == 0) {
            printf("%u.%u.%u\n",
                    globals.datetime.day,
                    globals.datetime.month,
                    globals.datetime.year);
        } else if (strcmp(buffer, "halt") == 0) {
            clear_screen();
            kprint("System halted.");
            halt();
        } else if (strcmp(buffer, "sysinfo") == 0) {
            kprint("System info:\n");
            printf("OS: %s\nVersion: %s\nCodename: %s\n",
                    globals.kernel_name,
                    globals.kernel_version,
                    globals.kernel_codename);
            for (int x = 0; x < 16; x++) {
                char y = ' ';
                kprint_attr(&y, x << 4);
                kprint_attr(&y, x << 4);
            }
            kprint("\n");
        } else if (strcmp(buffer, "beep") == 0) {
            speaker_beep();
        } else if (strcmp(buffer, "clear") == 0) {
            clear_screen();
        } else if (strcmp(buffer, "help") == 0) {
            kprint_attr("FrostOS Help:\n"
                        "====================\n"
                        "time - write current time to the screen\n"
                        "date - write current date to the screen\n"
                        "clear - clear screen\n"
                        "beep - a simple beep with the PC Speaker\n"
                        "sysinfo - get the system information\n"
                        "halt - halt the system\n"
                        "reboot - reboot the system\n"
                        "help - write out this help\n", GREEN_FG);
        } else if (strcmp(buffer, "reboot") == 0) {
            reboot();
        } else {
            kprint_attr("Unknown command: ", RED_FG);
            kprint_attr(buffer, RED_FG);
            kprint("\n");
        }
    }
}
