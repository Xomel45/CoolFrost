#ifndef NTFS_H
#define NTFS_H

#include <stdint.h>
#include "vfs.h"

/* ── NTFS Boot Sector (first sector of partition) ─────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem_id[8];             /* "NTFS    "                  */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  zeros1[3];
    uint16_t unused1;
    uint8_t  media_descriptor;
    uint16_t zeros2;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t unused2;
    uint32_t unused3;
    uint64_t total_sectors;
    uint64_t mft_cluster;           /* cluster number of $MFT      */
    uint64_t mft_mirror_cluster;
    int8_t   clusters_per_mft_record; /* if < 0, size = 2^(-val)   */
    uint8_t  unused4[3];
    int8_t   clusters_per_index_record;
    uint8_t  unused5[3];
    uint64_t volume_serial;
    uint32_t checksum;
} ntfs_boot_sector_t;

/* ── MFT Record header ────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    char     signature[4];          /* "FILE"                       */
    uint16_t fixup_offset;
    uint16_t fixup_count;
    uint64_t lsn;
    uint16_t sequence;
    uint16_t hard_link_count;
    uint16_t first_attr_offset;
    uint16_t flags;                 /* 0x01=in use, 0x02=directory  */
    uint32_t used_size;
    uint32_t allocated_size;
    uint64_t base_record;
    uint16_t next_attr_id;
} ntfs_mft_header_t;

/* ── Attribute header (common part, 16 bytes) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t type;                  /* 0x30=$FILE_NAME, 0x80=$DATA  */
    uint32_t length;                /* total attribute length        */
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
} ntfs_attr_common_t;

/* ── Resident attribute (after common header) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    ntfs_attr_common_t common;
    uint32_t value_length;
    uint16_t value_offset;
    uint16_t indexed_flag;
} ntfs_attr_resident_t;

/* ── Non-resident attribute (after common header) ─────────────────────── */
typedef struct __attribute__((packed)) {
    ntfs_attr_common_t common;
    uint64_t starting_vcn;
    uint64_t last_vcn;
    uint16_t data_runs_offset;
    uint16_t compression_unit;
    uint32_t padding;
    uint64_t allocated_size;
    uint64_t real_size;
    uint64_t initialized_size;
} ntfs_attr_nonresident_t;

/* ── $FILE_NAME attribute content (66+ bytes) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t parent_mft_ref;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_modification_time;
    uint64_t access_time;
    uint64_t allocated_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse;
    uint8_t  name_length;           /* in characters (UTF-16)      */
    uint8_t  name_namespace;        /* 0=POSIX,1=Win32,2=DOS,3=Both */
    /* uint16_t name[] follows — name_length chars of UTF-16LE */
} ntfs_filename_attr_t;

/* ── Index node header (inside $INDEX_ROOT / INDX records) ────────────── */
typedef struct __attribute__((packed)) {
    uint32_t first_entry_offset;    /* relative to this header      */
    uint32_t total_size;            /* relative to this header      */
    uint32_t allocated_size;
    uint32_t flags;                 /* 0x01 = has INDEX_ALLOCATION  */
} ntfs_index_node_header_t;

/* ── Index entry header (variable-length) ─────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t mft_reference;
    uint16_t entry_length;
    uint16_t content_length;
    uint32_t flags;                 /* 0x01=sub-node, 0x02=last     */
    /* if content_length > 0: ntfs_filename_attr_t follows */
} ntfs_index_entry_t;

/* $INDEX_ROOT attribute value header (before index node header) */
typedef struct __attribute__((packed)) {
    uint32_t attr_type;             /* type of indexed attr (0x30)  */
    uint32_t collation_rule;
    uint32_t index_alloc_size;      /* size of INDX records         */
    uint8_t  clusters_per_index_record;
    uint8_t  padding[3];
} ntfs_index_root_header_t;

/* ── Attribute type codes ─────────────────────────────────────────────── */
#define NTFS_ATTR_FILENAME          0x30
#define NTFS_ATTR_DATA              0x80
#define NTFS_ATTR_INDEX_ROOT        0x90
#define NTFS_ATTR_INDEX_ALLOCATION  0xA0
#define NTFS_ATTR_END               0xFFFFFFFF

/* ── MFT flags ────────────────────────────────────────────────────────── */
#define NTFS_MFT_IN_USE             0x01
#define NTFS_MFT_IS_DIRECTORY       0x02

/* ── File attribute flags ─────────────────────────────────────────────── */
#define NTFS_FILE_ATTR_HIDDEN       0x0002
#define NTFS_FILE_ATTR_SYSTEM       0x0004
#define NTFS_FILE_ATTR_DIRECTORY    0x10000000

/* ── Well-known MFT record numbers ───────────────────────────────────── */
#define NTFS_MFT_ROOT_DIR           5

/* ── Filename namespaces ──────────────────────────────────────────────── */
#define NTFS_NS_POSIX               0
#define NTFS_NS_WIN32               1
#define NTFS_NS_DOS                 2
#define NTFS_NS_WIN32_AND_DOS       3

/* ── Internal mounted state ───────────────────────────────────────────── */
typedef struct {
    uint8_t  drive;
    uint64_t part_lba;
    uint8_t  sectors_per_cluster;
    uint32_t cluster_size;
    uint64_t mft_lba;               /* absolute LBA of $MFT         */
    uint32_t mft_record_size;
    uint32_t mft_sectors;           /* sectors per MFT record        */
    uint32_t index_record_size;
} ntfs_fs_t;

/* ── Public API ───────────────────────────────────────────────────────── */
int         ntfs_mount(uint8_t drive, uint64_t part_lba, mount_point_t *mp);
dirent_t   *ntfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *ntfs_finddir(vfs_node_t *node, const char *name);
int         ntfs_read(vfs_node_t *node, uint64_t offset, uint32_t size,
                      void *buffer);

#endif
