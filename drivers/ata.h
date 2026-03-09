#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* ── I/O base ports ────────────────────────────────────────────────────── */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTRL  0x376

/* ── Register offsets from I/O base ────────────────────────────────────── */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01    /* read  */
#define ATA_REG_FEATURES    0x01    /* write */
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA_LO      0x03
#define ATA_REG_LBA_MID     0x04
#define ATA_REG_LBA_HI      0x05
#define ATA_REG_DRIVE       0x06
#define ATA_REG_STATUS      0x07    /* read  */
#define ATA_REG_COMMAND     0x07    /* write */

/* ── Control register offset from CTRL base ───────────────────────────── */
#define ATA_CTRL_ALTSTATUS  0x00    /* read  */
#define ATA_CTRL_DEVCTRL    0x00    /* write */

/* ── ATA commands ──────────────────────────────────────────────────────── */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY    0xEC

/* ── Status register bits ──────────────────────────────────────────────── */
#define ATA_SR_BSY          0x80    /* Busy                */
#define ATA_SR_DRDY         0x40    /* Drive ready         */
#define ATA_SR_DF           0x20    /* Drive write fault   */
#define ATA_SR_DSC          0x10    /* Drive seek complete  */
#define ATA_SR_DRQ          0x08    /* Data request ready  */
#define ATA_SR_CORR         0x04    /* Corrected data      */
#define ATA_SR_IDX          0x02    /* Index               */
#define ATA_SR_ERR          0x01    /* Error               */

/* ── Error register bits ───────────────────────────────────────────────── */
#define ATA_ER_BBK          0x80    /* Bad block           */
#define ATA_ER_UNC          0x40    /* Uncorrectable data  */
#define ATA_ER_IDNF         0x10    /* ID mark not found   */
#define ATA_ER_ABRT         0x04    /* Command aborted     */

/* ── Constants ─────────────────────────────────────────────────────────── */
#define ATA_SECTOR_SIZE     512
#define MAX_ATA_DRIVES      4       /* pri master/slave + sec master/slave */

/* ── MBR partition table entry (16 bytes) ──────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  boot_flag;         /* 0x80 = bootable, 0x00 = not */
    uint8_t  chs_start[3];     /* CHS address of first sector */
    uint8_t  type;              /* Partition type code         */
    uint8_t  chs_end[3];       /* CHS address of last sector  */
    uint32_t lba_start;         /* LBA of first sector         */
    uint32_t sector_count;      /* Total sectors in partition  */
} mbr_partition_t;

/* ── Drive info (populated by IDENTIFY) ────────────────────────────────── */
typedef struct {
    uint8_t  present;           /* 1 = drive detected          */
    char     model[41];         /* Model string (null-term)    */
    uint32_t sectors;           /* Total 28-bit LBA sectors    */
    uint16_t io_base;           /* I/O base port               */
    uint16_t ctrl_base;         /* Control base port           */
    uint8_t  is_slave;          /* 0 = master, 1 = slave       */
} ata_drive_t;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Initialize ATA: probes all 4 possible drives */
void ata_init(void);

/* Read `count` sectors starting at `lba` from drive `drive_idx` into `buffer`.
 * Returns 0 on success, negative on error. */
int ata_read_sectors(uint8_t drive_idx, uint32_t lba, uint8_t count, void *buffer);

/* Write `count` sectors starting at `lba` from `buffer` to drive `drive_idx`.
 * Returns 0 on success, negative on error. */
int ata_write_sectors(uint8_t drive_idx, uint32_t lba, uint8_t count, const void *buffer);

/* Get drive info struct (or NULL if index out of range) */
ata_drive_t *ata_get_drive(uint8_t index);

/* Number of detected drives */
uint8_t ata_drive_count(void);

/* Read MBR partition table from drive into parts[4].
 * Returns 0 on success, -1 on read error, -2 on bad MBR signature. */
int ata_read_partitions(uint8_t drive_idx, mbr_partition_t parts[4]);

/* Human-readable name for a partition type byte */
const char *partition_type_name(uint8_t type);

#endif
