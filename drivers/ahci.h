#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

typedef struct {
    uint8_t  port_idx;
    uint8_t  active;
    uint64_t sector_count;
} ahci_drive_t;

/* Scan PCI for AHCI controller, init all present SATA drives.
 * Returns 0 if at least one drive found, -1 if no AHCI controller. */
int ahci_init(void);

/* Read/write 512-byte sectors (count ≤ 64).  Returns 0 on success, -1 on error. */
int ahci_read_sectors (uint8_t drv, uint64_t lba, uint8_t count, void *buf);
int ahci_write_sectors(uint8_t drv, uint64_t lba, uint8_t count, const void *buf);

int           ahci_drive_count(void);
ahci_drive_t *ahci_get_drive(uint8_t idx);

#endif
