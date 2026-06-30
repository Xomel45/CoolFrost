#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

typedef struct {
    uint8_t  active;
    uint64_t sector_count;   /* total 512-byte sectors */
} vblk_drive_t;

/* Scan PCI for legacy virtio-blk (vendor=0x1AF4, device=0x1001).
 * Returns 0 if at least one drive found, -1 otherwise. */
int vblk_init(void);

int vblk_read_sectors (uint8_t drv, uint64_t lba, uint8_t count, void *buf);
int vblk_write_sectors(uint8_t drv, uint64_t lba, uint8_t count, const void *buf);

int           vblk_drive_count(void);
vblk_drive_t *vblk_get_drive(uint8_t idx);

#endif
