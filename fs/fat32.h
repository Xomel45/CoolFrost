#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "vfs.h"

/* ── FAT32 BPB (BIOS Parameter Block) ─────────────────────────────────── *
 * Occupies the first sector of a FAT32 partition.                         */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;      /* 0 for FAT32          */
    uint16_t total_sectors_16;      /* 0 for FAT32          */
    uint8_t  media_type;
    uint16_t fat_size_16;           /* 0 for FAT32          */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* ── FAT32 Extended ── */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;          /* usually 2            */
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];            /* "FAT32   "           */
} fat32_bpb_t;

/* ── FAT32 directory entry (32 bytes) ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    char     name[11];              /* 8.3 format, space-padded */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t access_date;
    uint16_t cluster_high;          /* high 16 bits of first cluster */
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;           /* low 16 bits of first cluster  */
    uint32_t size;
} fat32_dirent_t;

/* ── Directory entry attributes ────────────────────────────────────────── */
#define FAT32_ATTR_READ_ONLY    0x01
#define FAT32_ATTR_HIDDEN       0x02
#define FAT32_ATTR_SYSTEM       0x04
#define FAT32_ATTR_VOLUME_ID    0x08
#define FAT32_ATTR_DIRECTORY    0x10
#define FAT32_ATTR_ARCHIVE      0x20
#define FAT32_ATTR_LFN          0x0F    /* long filename marker */

/* ── FAT chain sentinel ────────────────────────────────────────────────── */
#define FAT32_EOC               0x0FFFFFF8

/* ── Internal state for a mounted FAT32 volume ─────────────────────────── */
typedef struct {
    uint8_t  drive;                 /* ATA drive index              */
    uint32_t part_lba;              /* partition start LBA          */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size;              /* sectors per FAT              */
    uint32_t root_cluster;          /* first cluster of root dir    */
    uint32_t fat_start_lba;         /* absolute LBA of first FAT    */
    uint32_t data_start_lba;        /* absolute LBA of cluster 2    */
    uint32_t total_sectors;
    char     volume_label[12];
} fat32_fs_t;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Mount a FAT32 partition.  Reads BPB, validates, fills mount_point_t.
 * Returns 0 on success, negative on error. */
int fat32_mount(uint8_t drive, uint32_t part_lba, mount_point_t *mp);

/* VFS callbacks — wired into vfs_node_t by fat32_mount / fat32_finddir */
dirent_t   *fat32_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *fat32_finddir(vfs_node_t *node, const char *name);
int         fat32_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                       void *buffer);

#endif
