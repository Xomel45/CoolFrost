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

/* ── GPT structures ───────────────────────────────────────────────────── */

/* 16-byte GUID stored as raw bytes (mixed-endian per UEFI spec) */
typedef struct __attribute__((packed)) {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} guid_t;

/* GPT header (LBA 1) — 92 bytes used, rest of 512 is reserved/zero */
typedef struct __attribute__((packed)) {
    char     signature[8];          /* "EFI PART"                  */
    uint32_t revision;              /* usually 0x00010000          */
    uint32_t header_size;           /* usually 92                  */
    uint32_t header_crc32;          /* CRC32 of header (0..header_size) */
    uint32_t reserved;              /* must be zero                */
    uint64_t current_lba;           /* LBA of this header (1)      */
    uint64_t backup_lba;            /* LBA of backup header        */
    uint64_t first_usable_lba;      /* first usable LBA for partitions */
    uint64_t last_usable_lba;       /* last usable LBA             */
    guid_t   disk_guid;             /* unique disk GUID            */
    uint64_t partition_entry_lba;   /* start LBA of partition entries */
    uint32_t num_partition_entries; /* number of entries (usually 128) */
    uint32_t partition_entry_size;  /* size of each entry (usually 128) */
    uint32_t partition_entries_crc32;
} gpt_header_t;

/* GPT partition entry — 128 bytes */
typedef struct __attribute__((packed)) {
    guid_t   type_guid;             /* partition type GUID         */
    guid_t   unique_guid;           /* unique partition GUID       */
    uint64_t first_lba;             /* starting LBA                */
    uint64_t last_lba;              /* ending LBA (inclusive)      */
    uint64_t attributes;            /* bit flags                   */
    uint16_t name[36];              /* UTF-16LE name (72 bytes)    */
} gpt_partition_entry_t;

/* Max GPT partitions we read (enough for typical disks) */
#define MAX_GPT_PARTS   32

/* Partition scheme detection */
#define PART_SCHEME_NONE  0
#define PART_SCHEME_MBR   1
#define PART_SCHEME_GPT   2

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

/* Detect partition scheme on a drive (PART_SCHEME_MBR or PART_SCHEME_GPT) */
uint8_t ata_detect_scheme(uint8_t drive_idx);

/* Read GPT partition table.  Returns number of valid entries found (0..max),
 * or negative on error.  `entries` must hold at least `max` entries. */
int ata_read_gpt(uint8_t drive_idx, gpt_partition_entry_t *entries, int max);

/* Human-readable name for a GPT partition type GUID */
const char *gpt_type_name(const guid_t *type);

/* Check if a GUID is all zeros (empty partition entry) */
int guid_is_zero(const guid_t *g);

#endif
