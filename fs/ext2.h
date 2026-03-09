#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include "vfs.h"

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2

/* Inode mode bits */
#define EXT2_S_IFDIR        0x4000
#define EXT2_S_IFREG        0x8000

/* Inode flags */
#define EXT4_EXTENTS_FL     0x00080000

/* Directory entry file types */
#define EXT2_FT_REG_FILE    1
#define EXT2_FT_DIR         2

/* ext4 extent magic */
#define EXT4_EXT_MAGIC      0xF30A

/* ── Superblock (at byte 1024 from partition start) ───────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;      /* block_size = 1024 << this     */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;               /* must be 0xEF53               */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* ── EXT2_DYNAMIC_REV fields ── */
    uint32_t s_first_ino;
    uint16_t s_inode_size;          /* 128 for old, 256 for ext4    */
} ext2_superblock_t;

/* ── Block group descriptor (32 bytes) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_group_desc_t;

/* ── Inode (128 bytes minimum) ────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;              /* in 512-byte units            */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];           /* 0-11 direct, 12 ind, 13 dind, 14 tind */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;           /* upper 32 bits (ext4)         */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

/* ── Directory entry (variable-length, header only) ───────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    /* char name[] follows */
} ext2_dir_entry_t;

/* ── ext4 extent tree structures ──────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t eh_magic;              /* 0xF30A                       */
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;              /* 0 = leaf node                */
    uint32_t eh_generation;
} ext4_extent_header_t;

typedef struct __attribute__((packed)) {
    uint32_t ee_block;              /* first logical block           */
    uint16_t ee_len;                /* number of blocks              */
    uint16_t ee_start_hi;           /* upper 16 bits of physical     */
    uint32_t ee_start_lo;           /* lower 32 bits of physical     */
} ext4_extent_t;

/* ── Internal mounted state ───────────────────────────────────────────── */
typedef struct {
    uint8_t  drive;
    uint32_t part_lba;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint16_t inode_size;
    uint32_t first_data_block;
    uint32_t groups_count;
} ext2_fs_t;

/* ── Public API ───────────────────────────────────────────────────────── */
int         ext2_mount(uint8_t drive, uint32_t part_lba, mount_point_t *mp);
dirent_t   *ext2_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *ext2_finddir(vfs_node_t *node, const char *name);
int         ext2_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                      void *buffer);

#endif
