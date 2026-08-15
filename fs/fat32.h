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
    uint8_t  drive;                 /* drive index                  */
    uint8_t  drive_type;            /* DRIVE_TYPE_ATA / DRIVE_TYPE_NVME */
    uint64_t part_lba;              /* partition start LBA          */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size;              /* sectors per FAT              */
    uint32_t root_cluster;          /* first cluster of root dir    */
    uint64_t fat_start_lba;         /* absolute LBA of first FAT    */
    uint64_t data_start_lba;        /* absolute LBA of cluster 2    */
    uint32_t total_sectors;
    uint32_t total_clusters;        /* data clusters, for fat32_alloc_cluster's
                                     * upper bound (highest valid cluster =
                                     * total_clusters + 1, since numbering
                                     * starts at 2) */
    uint32_t next_free_hint;        /* fat32_alloc_cluster's search start,
                                     * so repeated allocations (e.g. growing
                                     * one file by several clusters) don't
                                     * re-scan clusters already known used */
    char     volume_label[12];
} fat32_fs_t;

/* ── Per-node driver-private state ─────────────────────────────────────── *
 *
 * node->fs_private for every FAT32 vfs_node_t (files, directories, and the
 * volume root). Beyond the owning fat32_fs_t*, a file/directory obtained
 * via fat32_finddir/fat32_create also remembers WHERE its own 32-byte
 * directory entry lives on disk, so fat32_write can patch that entry's
 * size/cluster fields after growing the file — the volume root has no such
 * entry (it's referenced by the BPB's root_cluster field, not a dirent in
 * some parent directory), hence has_dirent.
 */
typedef struct {
    fat32_fs_t *fs;
    uint8_t     has_dirent;
    uint64_t    dirent_lba;         /* sector containing this node's dirent */
    uint16_t    dirent_off;         /* byte offset of the entry in that sector */
} fat32_file_info_t;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Mount a FAT32 partition.  Reads BPB, validates, fills mount_point_t.
 * Returns 0 on success, negative on error. */
int fat32_mount(uint8_t drive_type, uint8_t drive, uint64_t part_lba, mount_point_t *mp);

/* VFS callbacks — wired into vfs_node_t by fat32_mount / fat32_finddir */
dirent_t   *fat32_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *fat32_finddir(vfs_node_t *node, const char *name);
int         fat32_read(vfs_node_t *node, uint64_t offset, uint32_t size,
                       void *buffer);
int         fat32_write(vfs_node_t *node, uint64_t offset, uint32_t size,
                        const void *buffer);
vfs_node_t *fat32_create(vfs_node_t *dir, const char *name);
int         fat32_unlink(vfs_node_t *dir, const char *name);
int         fat32_rename(vfs_node_t *old_dir, const char *old_name,
                         vfs_node_t *new_dir, const char *new_name);

/* Defragments every regular file directly inside `dir_node` — see
 * fs/fat32.c's own doc comment for the move ordering that keeps this safe
 * against a crash/power loss mid-operation. Not recursive (this driver
 * has no mkdir, so volumes it created itself have no subdirectories to
 * descend into) and files-only (a subdirectory entry is left alone, not
 * an error). out_scanned, out_moved, out_skipped (any of which may be
 * NULL) are set to how many files were looked at, actually relocated, and
 * left in place for lack of a big-enough single free run, respectively.
 * Returns 0 on success (even if nothing needed moving), -1 if dir_node
 * isn't a valid FAT32 directory. */
int         fat32_defrag(vfs_node_t *dir_node, uint32_t *out_scanned,
                         uint32_t *out_moved, uint32_t *out_skipped);

#endif
