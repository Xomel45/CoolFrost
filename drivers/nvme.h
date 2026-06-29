#ifndef NVME_H
#define NVME_H

#include <stdint.h>

#define MAX_NVME_DRIVES  4

typedef struct {
    uint8_t  present;
    char     model[41];      /* model string, null-terminated */
    uint64_t sectors;        /* total capacity in 512-byte sectors */
    uint32_t lba_size;       /* logical block size in bytes */
    uint64_t bar0;           /* MMIO base address */
    uint32_t dstrd;          /* doorbell stride: stride = 4 << dstrd bytes */
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint8_t  admin_cq_phase;
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint8_t  io_cq_phase;
    uint16_t next_cid;
} nvme_drive_t;

/* Scan PCI, initialize all NVMe controllers found */
void          nvme_init(void);

/* Read/write in 512-byte sector units (matching ATA interface) */
int           nvme_read_sectors(uint8_t drv_idx, uint64_t lba, uint8_t count, void *buf);
int           nvme_write_sectors(uint8_t drv_idx, uint64_t lba, uint8_t count, const void *buf);

nvme_drive_t *nvme_get_drive(uint8_t idx);
uint8_t       nvme_drive_count(void);

#endif
