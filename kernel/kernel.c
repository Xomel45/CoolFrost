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
#include "../drivers/pci.h"
#include "../drivers/ata.h"
#include "../fs/vfs.h"
#include "../fs/fat32.h"
#include "../fs/ext2.h"
#include "../fs/ntfs.h"

#include <stdint.h>

char *buffer;
static kernel_globals globals;
static uint16_t *pci_devs;
static uint16_t total_devs = 0;
static uint8_t  cat_buf[512];

/* Check if `str` starts with `prefix` */
static int starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

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
}

kernel_globals *get_kernel_globals() {
    return &globals;
}

// Function that is called at the very start of the kernel to perform needed operations before, like initializing everything
void kstart(uintptr_t addr) {
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
    globals.kernel_name = "CoolFrost";
    globals.kernel_version = "0.1A";
    globals.kernel_codename = "Dark Rain";

    /* Init ATA and VFS */
    ata_init();
    vfs_init();

    perform_tests();
}

void pci_scan_devs() {
    for (uint16_t bus = 0; bus < 256; ++bus)
        for (uint8_t slot = 0; slot < 32; ++slot)
            if (pci_get_vendor(bus, slot) != 0xFFFF) {
                pci_devs[total_devs++] = (bus << 8) | slot;
            }
}

void kernel_main(uintptr_t magic, uintptr_t addr) {
    kstart(addr);
    sleep_ms(5000);
    clear_screen();

    KHEAPBM kheap;
    k_heapBMInit(&kheap);
    k_heapBMAddBlock(&kheap, (void *)0x800000, 0x100000, 16);
    buffer = (char *)k_heapBMAlloc(&kheap, 512);
    // TODO: FIX HEAP ALLOCATION
    pci_devs = (uint16_t *)(uintptr_t)0x900000; // Works but should use a heap allocator
    memset(pci_devs, 0, sizeof(uint16_t) * 256);
    printf("%s %s\n",
            globals.kernel_name,
            globals.kernel_version);
    pci_scan_devs();

    /* Auto-mount first suitable partition at /hda1 */
    if (ata_drive_count() > 0) {
        uint8_t scheme = ata_detect_scheme(0);
        int mounted = 0;

        if (scheme == PART_SCHEME_GPT) {
            gpt_partition_entry_t gparts[MAX_GPT_PARTS];
            int gn = ata_read_gpt(0, gparts, MAX_GPT_PARTS);
            for (int p = 0; p < gn && !mounted; p++) {
                uint32_t lba   = (uint32_t)gparts[p].first_lba;
                uint32_t count = (uint32_t)(gparts[p].last_lba - gparts[p].first_lba + 1);
                if (vfs_mount_gpt(0, lba, count, "/hda1") == 0) {
                    printf("Mounted GPT partition %d at /hda1\n", p);
                    mounted = 1;
                }
            }
        } else if (scheme == PART_SCHEME_MBR) {
            mbr_partition_t parts[4];
            if (ata_read_partitions(0, parts) == 0) {
                for (int p = 0; p < 4 && !mounted; p++) {
                    uint8_t pt = parts[p].type;
                    /* FAT32, FAT32-LBA, Linux (ext2/3/4), NTFS */
                    if (pt == 0x0B || pt == 0x0C ||
                        pt == 0x83 || pt == 0x07) {
                        if (vfs_mount(0, p, "/hda1") == 0) {
                            printf("Mounted MBR partition %d at /hda1\n", p);
                            mounted = 1;
                        }
                    }
                }
            }
        }

        if (mounted) {
            int fd = vfs_open("/hda1", 0);
            if (fd >= 0) {
                kprint("Files on /hda1:\n");
                for (uint32_t i = 0; ; i++) {
                    dirent_t *de = vfs_readdir(fd, i);
                    if (!de) break;
                    if (de->type == VFS_DIRECTORY)
                        printf("  [DIR]  %s\n", de->name);
                    else
                        printf("  %s  (%u bytes)\n", de->name, de->size);
                }
                vfs_close(fd);
            }
        }
    }

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
        } else if (strcmp(buffer, "ls") == 0) {
            int fd = vfs_open("/hda1", 0);
            if (fd < 0) {
                kprint_attr("No mounted filesystem\n", RED_FG);
            } else {
                for (uint32_t i = 0; ; i++) {
                    dirent_t *de = vfs_readdir(fd, i);
                    if (!de) break;
                    if (de->type == VFS_DIRECTORY)
                        printf("  [DIR]  %s\n", de->name);
                    else
                        printf("  %s  (%u bytes)\n", de->name, de->size);
                }
                vfs_close(fd);
            }
        } else if (starts_with(buffer, "cat ")) {
            const char *fname = buffer + 4;
            /* Skip leading spaces */
            while (*fname == ' ') fname++;
            if (!*fname) {
                kprint_attr("Usage: cat <filename>\n", RED_FG);
            } else {
                int fd = vfs_open(fname, 0);
                if (fd < 0) {
                    kprint_attr("File not found: ", RED_FG);
                    kprint((char *)fname);
                    kprint("\n");
                } else {
                    int n;
                    while ((n = vfs_read(fd, cat_buf, 511)) > 0) {
                        cat_buf[n] = '\0';
                        kprint((char *)cat_buf);
                    }
                    kprint("\n");
                    vfs_close(fd);
                }
            }
        } else if (strcmp(buffer, "lsblk") == 0) {
            uint8_t count = ata_drive_count();
            if (count == 0) {
                kprint("No ATA drives detected\n");
            } else {
                for (uint8_t i = 0; i < count; i++) {
                    ata_drive_t *drv = ata_get_drive(i);
                    if (!drv || !drv->present) continue;
                    uint32_t mb = drv->sectors / 2048;

                    uint8_t scheme = ata_detect_scheme(i);
                    const char *scheme_name = (scheme == PART_SCHEME_GPT) ? "GPT" :
                                              (scheme == PART_SCHEME_MBR) ? "MBR" : "???";
                    printf("ata%d: %s  %u MB  [%s]\n", i, drv->model, mb, scheme_name);

                    if (scheme == PART_SCHEME_GPT) {
                        gpt_partition_entry_t gparts[MAX_GPT_PARTS];
                        int gn = ata_read_gpt(i, gparts, MAX_GPT_PARTS);
                        for (int p = 0; p < gn; p++) {
                            uint32_t lba = (uint32_t)gparts[p].first_lba;
                            uint32_t secs = (uint32_t)(gparts[p].last_lba - gparts[p].first_lba + 1);
                            uint32_t pmb = secs / 2048;
                            printf("  p%d: %s  LBA %u  %u MB\n",
                                    p, gpt_type_name(&gparts[p].type_guid),
                                    lba, pmb);
                        }
                    } else if (scheme == PART_SCHEME_MBR) {
                        mbr_partition_t parts[4];
                        if (ata_read_partitions(i, parts) == 0) {
                            for (int p = 0; p < 4; p++) {
                                if (parts[p].type == 0x00) continue;
                                uint32_t pmb = parts[p].sector_count / 2048;
                                printf("  p%d: %s  LBA %u  %u MB\n",
                                        p, partition_type_name(parts[p].type),
                                        parts[p].lba_start, pmb);
                            }
                        }
                    }
                }
            }
        } else if (strcmp(buffer, "help") == 0) {
            kprint_attr("CoolFrost Help:\n"
                        "====================\n"
                        "time - write current time to the screen\n"
                        "date - write current date to the screen\n"
                        "clear - clear screen\n"
                        "beep - a simple beep with the PC Speaker\n"
                        "sysinfo - get the system information\n"
                        "halt - halt the system\n"
                        "reboot - reboot the system\n"
                        "help - write out this help\n"
                        "clock - a nice clock\n"
                        "charmap - character map of 437 Code Page\n"
                        "pciinfo - list of PCI devices\n"
                        "spkctl - PC speaker controller\n"
                        "ls - list files on mounted disk\n"
                        "cat <file> - show file contents\n"
                        "lsblk - list ATA drives and partitions\n", GREEN_FG);
        } else if (strcmp(buffer, "reboot") == 0) {
            reboot();
        } else if (strcmp(buffer, "charmap") == 0) {
            uint8_t cur_char = 1;
            uint8_t scancode = 0;
            clear_screen();
            printf("FrostOS Character Map v0.1\n");
            printf("Press 1/2 to select character\n");
            printf("Press ESC to quit\n");
            while (1) {
                set_cursor_offset(get_offset(0, 3));
                printf("%c", BoxCrnLeftUp);
                for (int i = 0; i < 6; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c\n%c", BoxCrnRightUp, BoxLineVert);
                if (cur_char == '\b') {
                    printf("0x%X  ", (int)cur_char);
                    kprint_backspace();
                    printf("%c\n", BoxLineVert);
                }
                else if (cur_char <= 0xF)
                    printf("0x%X  %c%c\n", (int)cur_char, (int)cur_char, BoxLineVert);
                else
                    printf("0x%X %c%c\n", (int)cur_char, (int)cur_char, BoxLineVert);

                printf("%c", BoxCrnLeftDown);
                for (int i = 0; i < 6; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c\n", BoxCrnRightDown);
                while (!(scancode = get_cur_scancode()) && (scancode < 0x04))
                    asm volatile("hlt");
                if (scancode == 0x01) break; // ESC pressed
                else if (scancode == 0x02) { // 1 pressed
                    if (cur_char != 1) {
                        cur_char--;
                        if (cur_char == '\n') cur_char--;
                    }
                } else if (scancode == 0x03) { // 2 pressed
                    if (cur_char != 255) {
                        cur_char++;
                        if (cur_char == '\n') cur_char++;
                    }
                }
                scancode = 0;
            }
        } else if (strcmp(buffer, "pciinfo") == 0) {
            uint8_t scancode;
            uint16_t cur_index = 0;
            clear_screen();
            printf("FrostOS PCI Info v0.1\n");
            printf("Press 1/2 to select device\n");
            printf("Press ESC to quit\n");
            while (1) {
                uint8_t bus = (pci_devs[cur_index] >> 8) & 0xFF;
                uint8_t slot = pci_devs[cur_index] & 0xFF;
                pci_base_device_header_t header = pci_get_base_device_header(bus, slot);

                set_cursor_offset(get_offset(0, 3));
                printf("%c", BoxCrnLeftUp);
                for (int i = 0; i < 78; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c", BoxCrnRightUp);

                printf("%cBus: 0x%X Slot: 0x%X\n", BoxLineVert, bus, slot);
                set_cursor_offset(get_offset(79, 4));
                printf("%c\n", BoxLineVert);

                printf("%cVendor: 0x%X Device ID: 0x%X", BoxLineVert, header.vendor, header.device_id);
                set_cursor_offset(get_offset(79, 5));
                printf("%c\n", BoxLineVert);

                printf("%cClass: 0x%X Subclass: 0x%X ProgIF: 0x%X Revision: 0x%X", BoxLineVert,
                        header.class_code, header.subclass, header.progif, header.revision_id);
                set_cursor_offset(get_offset(79, 6));
                printf("%c\n", BoxLineVert);

                printf("%cIO Space: %d Memory Space: %d Bus Master: %d Special Cycles: %d", BoxLineVert,
                        header.command.io_space, header.command.memory_space, header.command.bus_master,
                        header.command.special_cycles);
                set_cursor_offset(get_offset(79, 7));
                printf("%c\n", BoxLineVert);

                printf("%cMemory Write and Invalidate Enable: %d VGA Palette Snoop: %d", BoxLineVert,
                        header.command.mem_wr_inv_enable, header.command.vga_palette_snoop);
                set_cursor_offset(get_offset(79, 8));
                printf("%c\n", BoxLineVert);

                printf("%cParity Error Response: %d SERR# Enable: %d", BoxLineVert,
                        header.command.parity_err_response, header.command.serr_enable);
                set_cursor_offset(get_offset(79, 9));
                printf("%c\n", BoxLineVert);

                printf("%cFast Back2Back Enable: %d Interrupt Disable: %d", BoxLineVert,
                        header.command.fast_b2b_enable, header.command.interrupt_disable);
                set_cursor_offset(get_offset(79, 10));
                printf("%c\n", BoxLineVert);

                printf("%cInterrupt Status: %d Capabilities List: %d 66 MHz Capable: %d", BoxLineVert,
                        header.status.interrupt_status, header.status.capabilities_list,
                        header.status.mhz_capable);
                set_cursor_offset(get_offset(79, 11));
                printf("%c\n", BoxLineVert);

                printf("%cFast Back2Back Capable: %d Master Data Parity Error: %d", BoxLineVert,
                        header.status.fast_b2b_capable, header.status.master_data_parity_err);
                set_cursor_offset(get_offset(79, 12));
                printf("%c\n", BoxLineVert);

                printf("%cDEVSEL Timing: %d Signaled Target Abort: %d", BoxLineVert,
                        header.status.devsel_timing, header.status.signaled_target_abort);
                set_cursor_offset(get_offset(79, 13));
                printf("%c\n", BoxLineVert);

                printf("%cReceived Target Abort: %d Received Master Abort: %d", BoxLineVert,
                        header.status.received_target_abort, header.status.received_master_abort);
                set_cursor_offset(get_offset(79, 14));
                printf("%c\n", BoxLineVert);

                printf("%cSignaled System Error: %d Detected Parity Error: %d", BoxLineVert,
                        header.status.signaled_system_err, header.status.detected_parity_err);
                set_cursor_offset(get_offset(79, 15));
                printf("%c\n", BoxLineVert);

                printf("%cCache Line Size: 0x%X Latency Timer: 0x%X", BoxLineVert,
                        header.cache_line_size, header.latency_timer);
                set_cursor_offset(get_offset(79, 16));
                printf("%c\n", BoxLineVert);

                printf("%cHeader Type: %d BIST: 0x%X", BoxLineVert,
                        header.header_type, header.bist);
                set_cursor_offset(get_offset(79, 17));
                printf("%c\n", BoxLineVert);

                printf("%c", BoxCrnLeftDown);
                for (int i = 0; i < 78; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c", BoxCrnRightDown);

                while (!(scancode = get_cur_scancode()) && (scancode < 0x04))
                    asm volatile("hlt");
                if (scancode == 0x01) break; // ESC pressed
                else if (scancode == 0x02) { // 1 pressed
                    if (cur_index != 0) cur_index--;
                } else if (scancode == 0x03) { // 2 pressed
                    if ((cur_index + 1) != total_devs) cur_index++;
                }
                scancode = 0;
            }
        } else if (strcmp(buffer, "clock") == 0) {
            uint8_t scancode = 0;
            while (1) {
                clear_screen();
                printf("FrostOS Clock v0.1\n");
                printf("Press ESC to quit\n");

                set_cursor_offset(get_offset(0, 2));
                printf("%c", BoxCrnLeftUp);
                for (int i = 0; i < 19; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c\n", BoxCrnRightUp);

                printf("%cTime: %d:%d:%d", BoxLineVert, globals.datetime.hours, globals.datetime.minutes,
                        globals.datetime.seconds);
                set_cursor_offset(get_offset(20, 3));
                printf("%c\n", BoxLineVert);

                printf("%cDate: %d.%d.%d", BoxLineVert, globals.datetime.day, globals.datetime.month,
                        globals.datetime.year);
                set_cursor_offset(get_offset(20, 4));
                printf("%c\n", BoxLineVert);

                printf("%c", BoxCrnLeftDown);
                for (int i = 0; i < 19; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c\n", BoxCrnRightDown);
                
                sleep_ms(100);
                if ((scancode = get_cur_scancode()) == 0x01) break;
            }
        } else if (strcmp(buffer, "spkctl") == 0) {
            uint8_t scancode = 0;
            while (1) {
                clear_screen();
                printf("FrostOS PC Speaker Controller v0.1\n");
                printf("Press 1/2 to set frequency\n");
                printf("Press Enter to toggle speqker\n");
                printf("Press ESC to quit\n");
                set_cursor_offset(get_offset(0, 4));
                printf("%c", BoxCrnLeftUp);
                for (int i = 0; i < 20; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c\n", BoxCrnRightUp);
                printf("%cFrequency: %d Hz", BoxLineVert, speaker_get_freq());
                set_cursor_offset(get_offset(21, 5));
                printf("%c\n", BoxLineVert);
                printf("%cActive: %d", BoxLineVert, speaker_active());
                set_cursor_offset(get_offset(0, 6));
                printf("%c\n", BoxLineVert);
                printf("%c", BoxCrnLeftDown);
                for (int i = 0; i < 20; ++i)
                    printf("%c", BoxLineHorz);
                printf("%c\n", BoxCrnRightDown);
                
                while (!(scancode = get_cur_scancode()) && (scancode < 0x04))
                    asm volatile("hlt");
                if (scancode == 0x01) break; // ESC pressed
                else if (scancode == 0x1C) { // Enter pressed
                    if (!speaker_active()) {
                        if (speaker_get_freq() == 0) {
                            speaker_set_frequency(440);
                        } else speaker_set_frequency(speaker_get_freq());
                    } else {
                        speaker_stop();
                    }
                }
                else if (scancode == 0x02) { // 1 pressed
                    if (speaker_get_freq() != 20) {
                        if (!speaker_active()) {
                            speaker_set_frequency(speaker_get_freq()-1);
                            speaker_stop();
                        } else {
                            speaker_set_frequency(speaker_get_freq()-1);
                        }
                    }
                } else if (scancode == 0x03) { // 2 pressed
                    if (speaker_get_freq() != 2500) {
                        if (!speaker_active()) {
                            speaker_set_frequency(speaker_get_freq()+1);
                            speaker_stop();
                        } else {
                            speaker_set_frequency(speaker_get_freq()+1);
                        }
                    }
                }
                scancode = 0;
                
            }
        } else {
            kprint_attr("Unknown command: ", RED_FG);
            kprint_attr(buffer, RED_FG);
            kprint("\n");
        }
    }
}
