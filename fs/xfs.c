#include "xfs.h"
#include "../drivers/ata.h"
#include "../libc/mem.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Static pools — same fixed-array approach every other driver here uses.
 * ══════════════════════════════════════════════════════════════════════════ */

#define MAX_XFS_FS      4
#define MAX_XFS_NODES   128

static xfs_fs_t    fs_pool[MAX_XFS_FS];
static uint8_t     fs_pool_used = 0;

static vfs_node_t  node_pool[MAX_XFS_NODES];
static uint8_t     node_pool_used = 0;

/* inode_buf holds one raw on-disk inode (core + literal/fork area);
 * block_buf holds one raw fs block (directory data, or a file data block
 * being copied out by xfs_read). 4096 covers the largest inodesize/
 * blocksize this driver will encounter in practice (XFS inodes are
 * commonly 256 or 512 bytes; fs blocks up to a few KB for any volume this
 * environment can realistically mount) — sized the same way fs/ext2.c's
 * block_buf/meta_buf are, for the same reason. */
static uint8_t     inode_buf[4096];
static uint8_t     block_buf[4096];

static dirent_t    readdir_result;

static vfs_node_t *alloc_node(void) {
    if (node_pool_used >= MAX_XFS_NODES) return 0;
    return &node_pool[node_pool_used++];
}

static xfs_fs_t *alloc_fs(void) {
    if (fs_pool_used >= MAX_XFS_FS) return 0;
    return &fs_pool[fs_pool_used++];
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Big-endian field access — see fs/xfs.h's own doc comment for why this
 *  driver can't just overlay a packed struct and read fields directly the
 *  way every little-endian driver in this tree does.
 * ══════════════════════════════════════════════════════════════════════════ */

static inline uint16_t xfs_be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t xfs_be32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t xfs_be64(uint64_t v) { return __builtin_bswap64(v); }

/* ══════════════════════════════════════════════════════════════════════════
 *  Block / inode addressing
 * ══════════════════════════════════════════════════════════════════════════ */

static int read_block(xfs_fs_t *fs, uint64_t block, void *buf) {
    uint64_t lba = fs->part_lba + block * fs->sectors_per_block;
    return blk_read(fs->drive_type, fs->drive, lba, fs->sectors_per_block, buf);
}

/* fsbno (a "filesystem block number", the form extent records and the
 * superblock's own logstart/rootino-adjacent fields use) packs an AG
 * number into the high bits and an AG-relative block into the low
 * agblklog bits. The actual on-disk position adds agno*agblocks (the
 * volume's REAL per-AG stride), not agno<<agblklog (agblklog only has to
 * be wide enough to hold agblocks-1, it's not the stride itself — see
 * fs/xfs.h's mount-time validation of this relationship). */
static uint64_t xfs_fsb_to_block(xfs_fs_t *fs, uint64_t fsbno) {
    uint64_t agno  = fsbno >> fs->agblklog;
    uint64_t agbno = fsbno & (((uint64_t)1 << fs->agblklog) - 1);
    return agno * fs->agblocks + agbno;
}

/* Inode numbers use the identical AG-number/AG-relative-block packing as
 * fsbno, plus a further inopblog-wide offset for which of the several
 * inodes packed into that block this one is. */
static void xfs_ino_to_loc(xfs_fs_t *fs, uint64_t ino, uint64_t *out_block, uint32_t *out_off) {
    uint64_t agno   = ino >> (fs->agblklog + fs->inopblog);
    uint64_t rest   = ino & (((uint64_t)1 << (fs->agblklog + fs->inopblog)) - 1);
    uint64_t agbno  = rest >> fs->inopblog;
    uint32_t index  = (uint32_t)(rest & (((uint64_t)1 << fs->inopblog) - 1));

    *out_block = agno * fs->agblocks + agbno;
    *out_off   = index * fs->inodesize;
}

/* Reads inode `ino`'s raw bytes (inodesize of them) into inode_buf.
 * Returns 0 on success, -1 on I/O error or a bad magic (not actually an
 * inode — a corrupt inode number, or this driver's own addressing math
 * being wrong for this volume's geometry). */
static int read_inode_raw(xfs_fs_t *fs, uint64_t ino) {
    uint64_t block;
    uint32_t off;
    xfs_ino_to_loc(fs, ino, &block, &off);

    /* inodesize is always <= block_size in practice (multiple inodes
     * packed per block) for every volume this driver's addressing math
     * above assumes — read the whole containing block, then the inode is
     * a slice of it. block_buf would work just as well here, but sharing
     * it with directory-block reads that happen interleaved with inode
     * reads (readdir looks up each entry's inode right after reading the
     * directory block) would clobber one or the other; inode_buf keeps
     * them independent, same split reasoning as fs/ext2.c's block_buf vs
     * meta_buf vs ind_buf. */
    static uint8_t inode_block_buf[4096];
    if (read_block(fs, block, inode_block_buf) != 0) return -1;

    if (off + fs->inodesize > sizeof(inode_block_buf)) return -1;
    memcpy(inode_buf, &inode_block_buf[off], fs->inodesize);

    xfs_dinode_core_t *core = (xfs_dinode_core_t *)inode_buf;
    if (xfs_be16(core->di_magic) != XFS_DINODE_MAGIC) return -1;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Extent records — packed 128 bits (two big-endian uint64_t), NOT
 *  byte-aligned fields. Bit layout verified against a real image (see
 *  project memory): bit 63 of the first word is the extent flag, the next
 *  54 bits are the logical startoff, the low 9 bits of the first word plus
 *  the high 43 bits of the second word are the 52-bit physical startblock
 *  (an fsbno), and the low 21 bits of the second word are the block count.
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t startoff;
    uint64_t startblock;   /* fsbno — run through xfs_fsb_to_block before use */
    uint32_t blockcount;
    int      unwritten;
} xfs_extent_t;

static void xfs_decode_extent(const uint8_t *rec16, xfs_extent_t *out) {
    uint64_t l0 = xfs_be64(*(const uint64_t *)&rec16[0]);
    uint64_t l1 = xfs_be64(*(const uint64_t *)&rec16[8]);

    out->unwritten   = (int)(l0 >> 63);
    out->startoff    = (l0 >> 9) & (((uint64_t)1 << 54) - 1);
    out->startblock  = ((l0 & 0x1FFu) << 43) | (l1 >> 21);
    out->blockcount  = (uint32_t)(l1 & (((uint64_t)1 << 21) - 1));
}

/* Resolve logical block `lb` (within the DATA fork) to a physical block
 * number for an "extents" format fork whose records start at `ext_base`
 * (inode_buf + 100, the literal area) with `next` records available.
 * Returns 0 if `lb` falls in a hole (no such extent — sparse files aren't
 * otherwise expected from this driver's own test data, but a real-world
 * volume could have one; treated as "no data here" rather than guessed) or
 * past the last extent. */
static uint64_t xfs_resolve_extent(xfs_fs_t *fs, const uint8_t *ext_base,
                                   uint32_t next, uint64_t lb) {
    for (uint32_t i = 0; i < next; i++) {
        xfs_extent_t e;
        xfs_decode_extent(&ext_base[i * 16], &e);
        if (lb >= e.startoff && lb < e.startoff + e.blockcount) {
            uint64_t fsbno = e.startblock + (lb - e.startoff);
            return xfs_fsb_to_block(fs, fsbno);
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  xfs_read
 * ══════════════════════════════════════════════════════════════════════════ */

int xfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, void *buffer) {
    xfs_fs_t *fs = (xfs_fs_t *)node->fs_private;
    if (!fs) return -1;
    if (offset >= node->size) return 0;

    if (read_inode_raw(fs, node->inode) != 0) return -1;
    xfs_dinode_core_t *core = (xfs_dinode_core_t *)inode_buf;

    uint32_t remaining = size;
    if (offset + remaining > node->size) remaining = (uint32_t)(node->size - offset);
    if (remaining == 0) return 0;

    if (core->di_format == XFS_DINODE_FMT_LOCAL) {
        /* Whole file lives inline in the literal area — no block
         * resolution needed at all, just copy out of inode_buf. */
        memcpy(buffer, &inode_buf[100 + offset], remaining);
        return (int)remaining;
    }

    if (core->di_format != XFS_DINODE_FMT_EXTENTS) return -1;   /* btree fork: unsupported */

    uint32_t nextents = xfs_be32(core->di_nextents);
    const uint8_t *ext_base = &inode_buf[100];

    uint8_t *out = (uint8_t *)buffer;
    uint32_t got = 0;
    while (got < remaining) {
        uint64_t cur     = offset + got;
        uint64_t lb       = cur / fs->block_size;
        uint32_t blk_off  = (uint32_t)(cur % fs->block_size);

        uint64_t phys = xfs_resolve_extent(fs, ext_base, nextents, lb);
        if (phys == 0) break;   /* hole or past last extent */

        if (read_block(fs, phys, block_buf) != 0) break;

        uint32_t copy = fs->block_size - blk_off;
        if (copy > remaining - got) copy = remaining - got;
        memcpy(&out[got], &block_buf[blk_off], copy);
        got += copy;
    }

    return got == 0 ? -1 : (int)got;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Directory parsing — shortform (local) and single-block (extents,
 *  exactly one directory block) only, see fs/xfs.h's scope note.
 *
 *  Both fs_readdir_shortform/fs_walk_block below take a callback-free
 *  "give me the Nth entry" or "find this name" approach isn't shared
 *  between readdir/finddir the way fs/fat32.c's is — XFS's two on-disk
 *  layouts are different enough (inline byte stream vs. a whole block
 *  with a leaf/tail region) that walking them via one shared helper
 *  parameterized by "what to do with each entry" would obscure more than
 *  it'd save; each format gets its own small, direct walk in both
 *  xfs_readdir and xfs_finddir instead.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Shortform directory entry, decoded (never a fixed struct — namelen and
 * the parent/inumber width, 4 or 8 bytes depending on i8count, both vary
 * per-volume, verified against a real 3-entry directory). */
typedef struct {
    const char *name;
    int         namelen;
    uint64_t    inumber;
    uint8_t     ftype;
} xfs_sf_entry_t;

/* Walks a shortform directory's literal area (inode_buf + 100), calling
 * back via a plain linear scan the caller drives itself (both readdir and
 * finddir need slightly different things out of each entry, so this just
 * hands back one entry at a time by index rather than taking a callback).
 * Returns 1 and fills *out if entry `want_idx` exists, 0 if the directory
 * has fewer entries than that. */
static int xfs_sf_entry_at(xfs_fs_t *fs, uint32_t want_idx, xfs_sf_entry_t *out) {
    (void)fs;
    const uint8_t *sf = &inode_buf[100];
    uint8_t count    = sf[0];
    uint8_t i8count  = sf[1];
    uint32_t inum_sz = i8count ? 8 : 4;
    uint32_t pos     = 2 + inum_sz;   /* header: count(1) + i8count(1) + parent(4 or 8) */

    for (uint32_t i = 0; i < count; i++) {
        uint8_t namelen = sf[pos];
        const char *name = (const char *)&sf[pos + 3];   /* skip namelen(1) + offset(2) */
        uint8_t ftype = sf[pos + 3 + namelen];
        uint64_t inumber;
        if (inum_sz == 8) {
            uint64_t raw;
            memcpy(&raw, &sf[pos + 3 + namelen + 1], 8);
            inumber = xfs_be64(raw);
        } else {
            uint32_t raw;
            memcpy(&raw, &sf[pos + 3 + namelen + 1], 4);
            inumber = xfs_be32(raw);
        }

        if (i == want_idx) {
            out->name    = name;
            out->namelen = namelen;
            out->inumber = inumber;
            out->ftype   = ftype;
            return 1;
        }

        pos += 3 + namelen + 1 + inum_sz;
    }
    return 0;
}

/* Single directory-block ("XD2B") walk — same one-entry-by-index shape as
 * xfs_sf_entry_at above, operating on block_buf (the directory's one data
 * block, already read by the caller) instead of inode_buf. Skips "."/".."
 * (matches fs/ext2.c: ext2_readdir's own convention) and unused-space
 * filler entries (tag == 0xFFFF). */
static int xfs_block_entry_at(uint32_t dirblksize, uint32_t want_idx, xfs_sf_entry_t *out) {
    uint32_t tail_off = dirblksize - sizeof(xfs_dir2_block_tail_t);
    xfs_dir2_block_tail_t tail;
    memcpy(&tail, &block_buf[tail_off], sizeof(tail));
    uint32_t count = xfs_be32(tail.count);
    uint32_t leaf_start = tail_off - count * 8;

    uint32_t pos = sizeof(xfs_dir2_data_hdr_t);
    uint32_t seen = 0;
    while (pos < leaf_start) {
        uint16_t maybe_free;
        memcpy(&maybe_free, &block_buf[pos], 2);
        if (xfs_be16(maybe_free) == 0xFFFF) {
            uint16_t length;
            memcpy(&length, &block_buf[pos + 2], 2);
            pos += xfs_be16(length);
            continue;
        }

        uint64_t inumber_raw;
        memcpy(&inumber_raw, &block_buf[pos], 8);
        uint8_t namelen = block_buf[pos + 8];
        const char *name = (const char *)&block_buf[pos + 9];
        uint8_t ftype = block_buf[pos + 9 + namelen];
        uint32_t entry_len = (9 + namelen + 1 + 2 + 7) & ~7u;   /* + tag(2), padded to 8 */

        int is_dot    = (namelen == 1 && name[0] == '.');
        int is_dotdot = (namelen == 2 && name[0] == '.' && name[1] == '.');
        if (!is_dot && !is_dotdot) {
            if (seen == want_idx) {
                out->name    = name;
                out->namelen = namelen;
                out->inumber = xfs_be64(inumber_raw);
                out->ftype   = ftype;
                return 1;
            }
            seen++;
        }

        pos += entry_len;
    }
    return 0;
}

/* Loads `node`'s directory content ready for either walker above: for
 * "local" format nothing to do (inode_buf's literal area IS the shortform
 * directory, already loaded by read_inode_raw). For "extents" format,
 * confirms it's exactly the single-block case this driver supports and
 * reads that one block into block_buf. Returns 1 (shortform), 2
 * (single-block), or 0 (unsupported layout — multi-block, or a btree
 * fork; readdir/finddir just see an empty directory rather than guess). */
static int xfs_load_dir(vfs_node_t *node, xfs_fs_t *fs, uint32_t *out_dirblksize) {
    if (read_inode_raw(fs, node->inode) != 0) return 0;
    xfs_dinode_core_t *core = (xfs_dinode_core_t *)inode_buf;

    if (core->di_format == XFS_DINODE_FMT_LOCAL) return 1;
    if (core->di_format != XFS_DINODE_FMT_EXTENTS) return 0;

    uint32_t dirblksize = fs->block_size << fs->dirblklog;
    uint32_t nextents = xfs_be32(core->di_nextents);
    if (nextents != 1) return 0;   /* leaf/node/btree dir: not modeled */

    xfs_extent_t e;
    xfs_decode_extent(&inode_buf[100], &e);
    if (e.startoff != 0 || e.blockcount * fs->block_size != dirblksize) return 0;

    uint64_t phys = xfs_fsb_to_block(fs, e.startblock);
    if (dirblksize > sizeof(block_buf)) return 0;

    /* Multi-fs-block dir blocks (dirblklog > 0) are contiguous fs blocks
     * within the one extent — read them in one shot via a run of
     * sectors_per_block-sized reads, same "just call read_block per fs
     * block" as everywhere else in this driver (dirblksize is at most a
     * handful of fs blocks for any volume this test setup builds). */
    uint32_t fs_blocks_per_dirblk = 1u << fs->dirblklog;
    for (uint32_t i = 0; i < fs_blocks_per_dirblk; i++) {
        if (read_block(fs, phys + i, &block_buf[i * fs->block_size]) != 0) return 0;
    }

    if (out_dirblksize) *out_dirblksize = dirblksize;
    return 2;
}

dirent_t *xfs_readdir(vfs_node_t *node, uint32_t index) {
    xfs_fs_t *fs = (xfs_fs_t *)node->fs_private;
    if (!fs) return 0;

    uint32_t dirblksize = 0;
    int kind = xfs_load_dir(node, fs, &dirblksize);

    xfs_sf_entry_t e;
    int found;
    if (kind == 1) found = xfs_sf_entry_at(fs, index, &e);
    else if (kind == 2) found = xfs_block_entry_at(dirblksize, index, &e);
    else found = 0;

    if (!found) return 0;

    int len = e.namelen;
    if (len >= MAX_FILENAME) len = MAX_FILENAME - 1;
    memcpy(readdir_result.name, e.name, (size_t)len);
    readdir_result.name[len] = '\0';
    readdir_result.type = (e.ftype == 2) ? VFS_DIRECTORY : VFS_FILE;

    if (read_inode_raw(fs, e.inumber) == 0) {
        xfs_dinode_core_t *child = (xfs_dinode_core_t *)inode_buf;
        readdir_result.size = xfs_be64(child->di_size);
    } else {
        readdir_result.size = 0;
    }

    return &readdir_result;
}

vfs_node_t *xfs_finddir(vfs_node_t *node, const char *name) {
    xfs_fs_t *fs = (xfs_fs_t *)node->fs_private;
    if (!fs) return 0;

    int name_len = 0;
    while (name[name_len]) name_len++;

    uint32_t dirblksize = 0;
    int kind = xfs_load_dir(node, fs, &dirblksize);
    if (kind == 0) return 0;

    xfs_sf_entry_t e;
    for (uint32_t idx = 0; ; idx++) {
        int found = (kind == 1) ? xfs_sf_entry_at(fs, idx, &e)
                                 : xfs_block_entry_at(dirblksize, idx, &e);
        if (!found) return 0;

        if (e.namelen == name_len) {
            int match = 1;
            for (int i = 0; i < name_len; i++) {
                if (e.name[i] != name[i]) { match = 0; break; }
            }
            if (match) {
                if (read_inode_raw(fs, e.inumber) != 0) return 0;
                xfs_dinode_core_t *child = (xfs_dinode_core_t *)inode_buf;

                vfs_node_t *found_node = alloc_node();
                if (!found_node) return 0;

                int nl = name_len < MAX_FILENAME - 1 ? name_len : MAX_FILENAME - 1;
                memcpy(found_node->name, name, (size_t)nl);
                found_node->name[nl] = '\0';

                found_node->inode      = (uint32_t)e.inumber;
                found_node->size       = xfs_be64(child->di_size);
                found_node->fs_private = fs;
                found_node->parent     = node;

                uint16_t mode = xfs_be16(child->di_mode);
                if ((mode & XFS_S_IFMT) == XFS_S_IFDIR) {
                    found_node->type    = VFS_DIRECTORY;
                    found_node->readdir = xfs_readdir;
                    found_node->finddir = xfs_finddir;
                } else {
                    found_node->type = VFS_FILE;
                    found_node->read = xfs_read;
                }

                return found_node;
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  xfs_mount
 * ══════════════════════════════════════════════════════════════════════════ */

int xfs_mount(uint8_t drive_type, uint8_t drive, uint64_t part_lba, mount_point_t *mp) {
    uint8_t sb_raw[512];
    if (blk_read(drive_type, drive, part_lba, 1, sb_raw) != 0)
        return -1;

    xfs_dsb_t *sb = (xfs_dsb_t *)sb_raw;
    if (xfs_be32(sb->sb_magicnum) != XFS_SB_MAGIC)
        return -2;

    if ((xfs_be16(sb->sb_versionnum) & XFS_SB_VERSION_NUMBITS) == XFS_SB_VERSION_5)
        return -3;   /* V5 (CRC-enabled) format: not modeled, see fs/xfs.h */

    xfs_fs_t *fs = alloc_fs();
    if (!fs) return -4;

    fs->drive          = drive;
    fs->drive_type      = drive_type;
    fs->part_lba        = part_lba;
    fs->block_size       = xfs_be32(sb->sb_blocksize);
    fs->sectors_per_block = (uint8_t)(fs->block_size / 512);
    fs->agblocks         = xfs_be32(sb->sb_agblocks);
    fs->agcount           = xfs_be32(sb->sb_agcount);
    fs->agblklog          = sb->sb_agblklog;
    fs->inodesize         = xfs_be16(sb->sb_inodesize);
    fs->inopblock         = xfs_be16(sb->sb_inopblock);
    fs->inopblog          = sb->sb_inopblog;
    fs->dirblklog         = sb->sb_dirblklog;
    fs->rootino            = xfs_be64(sb->sb_rootino);

    if (fs->block_size == 0 || fs->block_size > sizeof(block_buf) ||
        fs->inodesize == 0 || fs->inodesize > sizeof(inode_buf))
        return -5;   /* geometry this driver's fixed buffers can't hold */

    vfs_node_t *root = alloc_node();
    if (!root) return -6;

    root->name[0]    = '/';
    root->name[1]    = '\0';
    root->type       = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root->inode      = (uint32_t)fs->rootino;
    root->size       = 0;
    root->parent     = 0;
    root->readdir    = xfs_readdir;
    root->finddir    = xfs_finddir;
    root->fs_private = fs;

    mp->root     = root;
    mp->drive    = drive;
    mp->fs_type  = FS_XFS;
    mp->part_lba = part_lba;
    mp->active   = 1;

    return 0;
}
