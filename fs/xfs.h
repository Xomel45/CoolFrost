#ifndef XFS_H
#define XFS_H

#include <stdint.h>
#include "vfs.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Read-only XFS driver.
 *
 *  Every multi-byte on-disk field in XFS is BIG-ENDIAN — unlike every other
 *  driver in this tree (FAT32/ext2/NTFS are all little-endian, matching
 *  x86_64, so they can overlay a packed struct directly onto disk bytes and
 *  read fields as-is). The structs below still mirror the on-disk layout
 *  byte-for-byte (verified against a real mkfs.xfs image, field by field,
 *  not just from spec reading), but every access site in fs/xfs.c has to
 *  run the field through xfs_be16/32/64 explicitly — there is no shortcut
 *  around that here.
 *
 *  Scope, chosen deliberately narrow for a first read-only implementation:
 *   - Only the classic non-CRC (V4, versionnum V4/V4+ATTR/etc., not V5)
 *     on-disk format is supported. V5 adds a CRC32C + LSN + UUID header to
 *     nearly every metadata structure (superblock, AG headers, inodes,
 *     directory blocks, ...) — real, but a second on-disk format to
 *     validate throughout, not just a field or two, and V4 is still real
 *     XFS, still what `mkfs.xfs -m crc=0` produces today (with a
 *     deprecation warning, not a removal). sb_versionnum's top bits are
 *     checked at mount and a V5 volume is refused outright rather than
 *     silently misread.
 *   - Inode data fork formats: "local" (tiny files/directories, inline in
 *     the inode's literal area) and "extents" (a flat array of extent
 *     records in the literal area) only. "btree" format (too many extents
 *     to fit inline) is refused — same spirit as fs/ext2.c refusing
 *     double/triple indirect and ext4 extent-tree depth for its own write
 *     path: a real, documented limit, not a silent wrong answer.
 *   - Directory formats: "local" (shortform, inline) and a single-block
 *     "extents" layout (the whole directory fits in one directory block)
 *     only. Multi-block (leaf/node/btree) directories are NOT walked —
 *     readdir/finddir just find nothing beyond what shortform/single-block
 *     parsing covers, rather than attempt to parse a hash-indexed btree
 *     this driver doesn't model. This driver's own vfs_mount test image is
 *     built specifically to stay within this — see project memory for how
 *     that was verified against a real, much larger directory too.
 *   - No allocation group free-space (AGF) or inode (AGI) btrees are read
 *     at all — read-only never needs to find free space or iterate every
 *     inode, and a specific inode's location is computable directly from
 *     its number (xfs_ino_to_loc), so neither structure is ever consulted.
 *   - Inode numbers are truncated to 32 bits at the vfs_node_t boundary
 *     (vfs.h: vfs_node_t.inode is uint32_t, shared by every driver here,
 *     not something worth widening just for this one) — fine for any
 *     volume small enough to actually reach in this environment, but a
 *     real multi-TB XFS filesystem with enough AGs can exceed that.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Superblock (verified field-by-field against a real image; only the
 * prefix this driver actually needs, through sb_dirblklog) ────────────── */
typedef struct __attribute__((packed)) {
    uint32_t sb_magicnum;        /* 0   "XFSB" */
    uint32_t sb_blocksize;       /* 4 */
    uint64_t sb_dblocks;         /* 8 */
    uint64_t sb_rblocks;         /* 16 */
    uint64_t sb_rextents;        /* 24 */
    uint8_t  sb_uuid[16];        /* 32 */
    uint64_t sb_logstart;        /* 48 */
    uint64_t sb_rootino;         /* 56 */
    uint64_t sb_rbmino;          /* 64 */
    uint64_t sb_rsumino;         /* 72 */
    uint32_t sb_rextsize;        /* 80 */
    uint32_t sb_agblocks;        /* 84 */
    uint32_t sb_agcount;         /* 88 */
    uint32_t sb_rbmblocks;       /* 92 */
    uint32_t sb_logblocks;       /* 96 */
    uint16_t sb_versionnum;      /* 100 */
    uint16_t sb_sectsize;        /* 102 */
    uint16_t sb_inodesize;       /* 104 */
    uint16_t sb_inopblock;       /* 106 */
    char     sb_fname[12];       /* 108 */
    uint8_t  sb_blocklog;        /* 120 */
    uint8_t  sb_sectlog;         /* 121 */
    uint8_t  sb_inodelog;        /* 122 */
    uint8_t  sb_inopblog;        /* 123 */
    uint8_t  sb_agblklog;        /* 124 */
    uint8_t  sb_rextslog;        /* 125 */
    uint8_t  sb_inprogress;      /* 126 */
    uint8_t  sb_imax_pct;        /* 127 */
    uint64_t sb_icount;          /* 128 */
    uint64_t sb_ifree;           /* 136 */
    uint64_t sb_fdblocks;        /* 144 */
    uint64_t sb_frextents;       /* 152 */
    uint64_t sb_uquotino;        /* 160 */
    uint64_t sb_gquotino;        /* 168 */
    uint16_t sb_qflags;          /* 176 */
    uint8_t  sb_flags;           /* 178 */
    uint8_t  sb_shared_vn;       /* 179 */
    uint32_t sb_inoalignmt;      /* 180 */
    uint32_t sb_unit;            /* 184 */
    uint32_t sb_width;           /* 188 */
    uint8_t  sb_dirblklog;       /* 192 */
} xfs_dsb_t;

#define XFS_SB_MAGIC 0x58465342u   /* "XFSB" */

/* sb_versionnum low nibble = version number; bit 0x8000 (ATTR)/0x1000
 * (NUM/version 5 marker area) etc. are feature flags layered on top. A V5
 * filesystem is identified by the low nibble being 5 — refused outright,
 * see fs/xfs.h's own scope note above. */
#define XFS_SB_VERSION_NUMBITS 0x000fu
#define XFS_SB_VERSION_5       5u

/* ── Inode core + next_unlinked (100 bytes, verified byte-for-byte —
 * literal/fork area starts immediately after, at byte 100) ────────────── */
typedef struct __attribute__((packed)) {
    uint16_t di_magic;           /* 0    "IN" */
    uint16_t di_mode;            /* 2 */
    int8_t   di_version;         /* 4 */
    int8_t   di_format;          /* 5 */
    uint16_t di_onlink;          /* 6 */
    uint32_t di_uid;             /* 8 */
    uint32_t di_gid;             /* 12 */
    uint32_t di_nlink;           /* 16 */
    uint16_t di_projid_lo;       /* 20 */
    uint16_t di_projid_hi;       /* 22 */
    uint8_t  di_pad[6];          /* 24 */
    uint16_t di_flushiter;       /* 30 */
    uint32_t di_atime_sec;       /* 32 */
    uint32_t di_atime_nsec;      /* 36 */
    uint32_t di_mtime_sec;       /* 40 */
    uint32_t di_mtime_nsec;      /* 44 */
    uint32_t di_ctime_sec;       /* 48 */
    uint32_t di_ctime_nsec;      /* 52 */
    uint64_t di_size;            /* 56 */
    uint64_t di_nblocks;         /* 64 */
    uint32_t di_extsize;         /* 72 */
    uint32_t di_nextents;        /* 76 */
    uint16_t di_anextents;       /* 80 */
    uint8_t  di_forkoff;         /* 82 */
    int8_t   di_aformat;         /* 83 */
    uint32_t di_dmevmask;        /* 84 */
    uint16_t di_dmstate;         /* 88 */
    uint16_t di_flags;           /* 90 */
    uint32_t di_gen;             /* 92 */
    uint32_t di_next_unlinked;   /* 96 */
} xfs_dinode_core_t;

#define XFS_DINODE_MAGIC 0x494eu   /* "IN" */

#define XFS_DINODE_FMT_LOCAL   1
#define XFS_DINODE_FMT_EXTENTS 2
#define XFS_DINODE_FMT_BTREE   3

/* di_mode: standard POSIX S_IFDIR/S_IFREG bits, same values fs/ext2.h's
 * EXT2_S_IFDIR uses — not shared/#included from there on purpose, same
 * "each driver owns its own on-disk constants" convention as everywhere
 * else in this tree. */
#define XFS_S_IFMT  0xF000u
#define XFS_S_IFDIR 0x4000u
#define XFS_S_IFREG 0x8000u

/* ── Block-format (single-block) directory header + tail — entries in
 * between are variable-length, parsed by hand in fs/xfs.c, not a fixed
 * struct. Verified against a real 62-entry directory. ─────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;               /* "XD2B" (non-CRC single-block dir) */
    struct { uint16_t offset; uint16_t length; } bestfree[3];
} xfs_dir2_data_hdr_t;             /* 16 bytes */

#define XFS_DIR2_BLOCK_MAGIC 0x58443242u   /* "XD2B" */

typedef struct __attribute__((packed)) {
    uint32_t count;
    uint32_t stale;
} xfs_dir2_block_tail_t;

/* ── Internal state for a mounted XFS volume ───────────────────────────── */
typedef struct {
    uint8_t  drive;
    uint8_t  drive_type;
    uint64_t part_lba;
    uint32_t block_size;          /* bytes */
    uint8_t  sectors_per_block;
    uint32_t agblocks;
    uint32_t agcount;
    uint8_t  agblklog;
    uint16_t inodesize;
    uint16_t inopblock;
    uint8_t  inopblog;
    uint8_t  dirblklog;           /* dir block size = block_size << dirblklog */
    uint64_t rootino;
} xfs_fs_t;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Mount an XFS partition (read-only). Returns 0 on success, negative on
 * error (bad magic, unsupported V5 format, or an I/O error). */
int         xfs_mount(uint8_t drive_type, uint8_t drive, uint64_t part_lba, mount_point_t *mp);

/* VFS callbacks — wired into vfs_node_t by xfs_mount / xfs_finddir. No
 * write/create/unlink/rename — this driver is read-only, full stop. */
dirent_t   *xfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *xfs_finddir(vfs_node_t *node, const char *name);
int         xfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, void *buffer);

#endif
