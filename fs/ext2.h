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
    uint16_t s_block_group_nr;      /* which group THIS copy of the sb is in */
    uint32_t s_feature_compat;      /* safe to ignore either way             */
    uint32_t s_feature_incompat;    /* unknown bit here → refuse to mount at
                                     * all, even read-only (layout itself is
                                     * unsafe to interpret — see EXT2_SUPPORTED_INCOMPAT) */
    uint32_t s_feature_ro_compat;   /* unknown bit here → mount OK, but this
                                     * driver's write path must refuse       */
} ext2_superblock_t;

/* ── s_state ─────────────────────────────────────────────────────────── */
#define EXT2_VALID_FS  0x0001   /* cleanly unmounted last time              */
#define EXT2_ERROR_FS  0x0002   /* errors were detected                     */

/* ── s_feature_incompat bits this driver actually understands ─────────── *
 *
 * Per the ext2/3/4 spec, an UNRECOGNIZED bit here means the on-disk layout
 * itself may not mean what this driver thinks it means (group descriptor
 * size, indirect-vs-extent block mapping, whether a journal has pending
 * unreplayed transactions, ...) — mounting anyway, even read-only, risks
 * silently misinterpreting the filesystem. This driver refuses the WHOLE
 * mount if it sees anything outside this set (fs/ext2.c: ext2_mount).
 *
 * Notably NOT in the whitelist, on purpose: EXT3_FEATURE_INCOMPAT_RECOVER
 * (0x0004 — journal has pending transactions this driver can't replay;
 * writing over an unreplayed journal risks real corruption), 64BIT (0x0080
 * — group descriptors double in size; this driver's ext2_group_desc_t is
 * hardcoded to the 32-byte layout, so group 1+ lookups would already have
 * silently landed on the wrong block before this check existed), META_BG,
 * MMP, FLEX_BG, EA_INODE, DIRDATA, INLINE_DATA, ENCRYPT, and anything else. */
#define EXT2_FEATURE_INCOMPAT_FILETYPE  0x0002  /* dirent file_type byte — always used here */
#define EXT4_FEATURE_INCOMPAT_EXTENTS   0x0040  /* read-only support, fs/ext2.c: ext4_extent_lookup */
#define EXT2_SUPPORTED_INCOMPAT \
    (EXT2_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS)

/* ── s_feature_ro_compat bits safe to WRITE under ──────────────────────── *
 *
 * Unlike incompat, an unrecognized ro_compat bit is safe to mount (even the
 * kernel spec says so — "ro" means old drivers degrade to read-only, not
 * "refuse entirely"), so ext2_mount always succeeds regardless. But writing
 * WITHOUT understanding one of these can leave its bookkeeping stale in a
 * way real tools then see as corruption — most importantly METADATA_CSUM
 * (0x0400): every bitmap/inode/dirent/extent block carries a crc32c this
 * driver doesn't know how to compute, so any write leaves a mismatched
 * checksum. Also deliberately NOT whitelisted: GDT_CSUM/uninit_bg (0x0010)
 * — group descriptors gain a crc16 + lazy-init flags this driver doesn't
 * touch, and it's genuinely unclear whether an "uninitialized" group's
 * on-disk bitmap is safe to read/allocate from without handling that.
 *
 * SPARSE_SUPER/LARGE_FILE/HUGE_FILE/DIR_NLINK, by contrast, only change how
 * OTHER inodes may be shaped (files >2GB/16TB, directories with huge link
 * counts) — this driver never creates such inodes itself and never reads
 * i_blocks for anything correctness-critical, so they don't force any
 * behavior on code that doesn't opt in. LARGE_FILE's actual risk (mis-
 * reading an EXISTING file's real size if it's already >4GB) is guarded
 * per-inode instead, at the point of use — see ext2_write's i_size_high
 * check — rather than refusing the entire volume, since practically every
 * real ext2/3/4 filesystem sets LARGE_FILE regardless of whether any file
 * on it is actually large.
 *
 * fs->write_supported (set in ext2_mount) gates ext2_create/ext2_write on
 * this — an unsupported bit here means read-only, not refuse to mount, so
 * existing read-only behavior on such volumes is unaffected. */
#define EXT2_FEATURE_ROCOMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_ROCOMPAT_LARGE_FILE   0x0002
#define EXT4_FEATURE_ROCOMPAT_HUGE_FILE    0x0008
#define EXT4_FEATURE_ROCOMPAT_DIR_NLINK    0x0020
#define EXT2_SUPPORTED_ROCOMPAT_FOR_WRITE \
    (EXT2_FEATURE_ROCOMPAT_SPARSE_SUPER | EXT2_FEATURE_ROCOMPAT_LARGE_FILE | \
     EXT4_FEATURE_ROCOMPAT_HUGE_FILE | EXT4_FEATURE_ROCOMPAT_DIR_NLINK)

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

/* Internal (index) node entry — points to a child extent tree block */
typedef struct __attribute__((packed)) {
    uint32_t ei_block;              /* covers logical blocks >= this */
    uint32_t ei_leaf_lo;            /* lower 32 bits of child block  */
    uint16_t ei_leaf_hi;            /* upper 16 bits of child block  */
    uint16_t ei_unused;
} ext4_extent_idx_t;

/* ── Internal mounted state ───────────────────────────────────────────── */
typedef struct {
    uint8_t  drive;
    uint8_t  drive_type;   /* DRIVE_TYPE_ATA / DRIVE_TYPE_NVME */
    uint64_t part_lba;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint16_t inode_size;
    uint32_t first_data_block;
    uint32_t groups_count;
    uint32_t blocks_count;  /* sb->s_blocks_count — total blocks, for the last
                             * group's bitmap bound (it's usually short) */
    uint32_t inodes_count;  /* sb->s_inodes_count */
    uint32_t first_ino;     /* sb->s_first_ino (11 for old-rev volumes, which
                             * don't carry the field) — alloc_inode() must
                             * never hand out one of the reserved inodes
                             * below this (root=2, journal=8, ...) */
    uint8_t  write_supported;  /* 0 → ext2_write/ext2_create refuse outright.
                                * Set in ext2_mount from s_feature_ro_compat
                                * (EXT2_SUPPORTED_ROCOMPAT_FOR_WRITE) and
                                * s_state (EXT2_VALID_FS) — see their doc
                                * comments for why. Mounting itself always
                                * succeeds regardless; this only gates writes. */
} ext2_fs_t;

/* ── Public API ───────────────────────────────────────────────────────── */
int         ext2_mount(uint8_t drive_type, uint8_t drive, uint64_t part_lba, mount_point_t *mp);
dirent_t   *ext2_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *ext2_finddir(vfs_node_t *node, const char *name);
int         ext2_read(vfs_node_t *node, uint64_t offset, uint32_t size,
                      void *buffer);
int         ext2_write(vfs_node_t *node, uint64_t offset, uint32_t size,
                       const void *buffer);
vfs_node_t *ext2_create(vfs_node_t *dir, const char *name);
int         ext2_unlink(vfs_node_t *dir, const char *name);
int         ext2_rename(vfs_node_t *old_dir, const char *old_name,
                        vfs_node_t *new_dir, const char *new_name);

/* Defragments every regular file directly inside `dir_node` — see
 * fs/ext2.c's own doc comment for the move ordering and scope limits
 * (direct + single indirect blocks only, same as get_or_alloc_block; ext4
 * extents and double/triple indirect files are skipped, not an error).
 * Not recursive. out_scanned, out_moved, out_skipped (any of which may be
 * NULL) are set the same way fat32_defrag's are. Returns 0 on success
 * (even if nothing needed moving), -1 if dir_node isn't a valid ext2
 * directory. */
int         ext2_defrag(vfs_node_t *dir_node, uint32_t *out_scanned,
                        uint32_t *out_moved, uint32_t *out_skipped);

#endif
