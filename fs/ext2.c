#include "ext2.h"
#include "../drivers/ata.h"
#include "../libc/mem.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Static pools & buffers
 * ══════════════════════════════════════════════════════════════════════════ */

#define MAX_EXT2_FS     4
#define MAX_EXT2_NODES  128

static ext2_fs_t   fs_pool[MAX_EXT2_FS];
static uint8_t     fs_pool_used = 0;

static vfs_node_t  node_pool[MAX_EXT2_NODES];
static uint8_t     node_pool_used = 0;

/* Three separate buffers to avoid clobbering:
 *   block_buf  — directory / file data blocks
 *   meta_buf   — metadata reads (BGD, inode table)
 *   ind_buf    — indirect block pointer reads         */
static uint8_t     block_buf[4096];
static uint8_t     meta_buf[4096];
static uint8_t     ind_buf[4096];
static uint8_t     bitmap_buf[4096];  /* block/inode bitmap scans (alloc_block/alloc_inode) */

/* ext2_defrag_one's old-block-list scratch space — sized for the largest
 * block_size these buffers above support (4096, giving 4096/4 = 1024
 * indirect pointers) plus the 12 direct slots. Static rather than a large
 * stack array, matching every other per-operation scratch buffer here. */
static uint32_t    old_blocks_buf[12 + 1024];

static dirent_t    readdir_result;

/* ══════════════════════════════════════════════════════════════════════════
 *  Allocators
 * ══════════════════════════════════════════════════════════════════════════ */

static vfs_node_t *alloc_node(void) {
    if (node_pool_used >= MAX_EXT2_NODES) return 0;
    vfs_node_t *n = &node_pool[node_pool_used++];
    memset(n, 0, sizeof(vfs_node_t));
    return n;
}

static ext2_fs_t *alloc_fs(void) {
    if (fs_pool_used >= MAX_EXT2_FS) return 0;
    ext2_fs_t *f = &fs_pool[fs_pool_used++];
    memset(f, 0, sizeof(ext2_fs_t));
    return f;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Low-level helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Read one full block into `buf` */
static int read_block(ext2_fs_t *fs, uint32_t block, void *buf) {
    uint64_t lba = fs->part_lba + (uint64_t)block * fs->sectors_per_block;
    return blk_read(fs->drive_type, fs->drive, lba,
                    (uint8_t)fs->sectors_per_block, buf);
}

/* Write one full block from `buf` */
static int write_block(ext2_fs_t *fs, uint32_t block, const void *buf) {
    uint64_t lba = fs->part_lba + (uint64_t)block * fs->sectors_per_block;
    return blk_write(fs->drive_type, fs->drive, lba,
                     (uint8_t)fs->sectors_per_block, buf);
}

/* Where group `group`'s 32-byte descriptor lives: which block of the block
 * group descriptor table, and the byte offset within it. Shared by every
 * function that needs to read OR write a group descriptor. */
static void locate_bgd(ext2_fs_t *fs, uint32_t group, uint32_t *out_blk, uint32_t *out_off) {
    uint32_t bgdt_block = fs->first_data_block + 1;
    uint32_t bgd_byte   = group * sizeof(ext2_group_desc_t);
    *out_blk = bgdt_block + bgd_byte / fs->block_size;
    *out_off = bgd_byte % fs->block_size;
}

/* Where inode `ino` lives within its group's inode table (already located):
 * which block, and the byte offset within it. */
static void locate_inode(ext2_fs_t *fs, uint32_t inode_table, uint32_t index,
                         uint32_t *out_blk, uint32_t *out_off) {
    uint32_t inode_byte = index * fs->inode_size;
    *out_blk = inode_table + inode_byte / fs->block_size;
    *out_off = inode_byte % fs->block_size;
}

/* Read an inode by number.  Uses meta_buf internally. */
static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    uint32_t bgd_blk, bgd_off;
    locate_bgd(fs, group, &bgd_blk, &bgd_off);
    if (read_block(fs, bgd_blk, meta_buf) != 0)
        return -1;

    ext2_group_desc_t *bgd = (ext2_group_desc_t *)&meta_buf[bgd_off];
    uint32_t inode_table = bgd->bg_inode_table;

    uint32_t inode_blk, inode_off;
    locate_inode(fs, inode_table, index, &inode_blk, &inode_off);
    if (read_block(fs, inode_blk, meta_buf) != 0)
        return -1;

    memcpy((uint8_t *)out, &meta_buf[inode_off], (int)sizeof(ext2_inode_t));
    return 0;
}

/* Write an inode by number back — read-modify-write of the block it lives
 * in (128/256-byte inodes pack several per block, can't overwrite the
 * whole block). Uses meta_buf internally, same clobber-safe ordering as
 * read_inode (bgd fields copied out before the buffer is reused). */
static int write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in) {
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    uint32_t bgd_blk, bgd_off;
    locate_bgd(fs, group, &bgd_blk, &bgd_off);
    if (read_block(fs, bgd_blk, meta_buf) != 0)
        return -1;

    ext2_group_desc_t *bgd = (ext2_group_desc_t *)&meta_buf[bgd_off];
    uint32_t inode_table = bgd->bg_inode_table;

    uint32_t inode_blk, inode_off;
    locate_inode(fs, inode_table, index, &inode_blk, &inode_off);
    if (read_block(fs, inode_blk, meta_buf) != 0)
        return -1;

    memcpy(&meta_buf[inode_off], (const uint8_t *)in, (int)sizeof(ext2_inode_t));
    return write_block(fs, inode_blk, meta_buf);
}

/* Read-modify-write the superblock's free block/inode counters. Called once
 * per allocation (not batched) — fine at this driver's scale, but a bulk
 * allocator would want to accumulate deltas and flush once. */
static void sb_adjust_free(ext2_fs_t *fs, int32_t blocks_delta, int32_t inodes_delta) {
    uint8_t sb_raw[1024];
    if (blk_read(fs->drive_type, fs->drive, fs->part_lba + 2, 2, sb_raw) != 0)
        return;
    ext2_superblock_t *sb = (ext2_superblock_t *)sb_raw;
    sb->s_free_blocks_count += (uint32_t)blocks_delta;
    sb->s_free_inodes_count += (uint32_t)inodes_delta;
    blk_write(fs->drive_type, fs->drive, fs->part_lba + 2, 2, sb_raw);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Bitmap allocators
 *
 *  alloc_block/alloc_inode scan every block group from 0 until one has a
 *  free bit, flip it, and update the group descriptor's + superblock's free
 *  counters to match. alloc_block_near (below alloc_block) sits in front of
 *  alloc_block for the common growth case — get_or_alloc_block calls it
 *  with a "one past the previous block" goal so a file growing block-by-
 *  block stays physically contiguous, instead of every new block landing
 *  wherever alloc_block's from-group-0 scan finds the lowest free bit.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Find and reserve one free data block. Returns the absolute block number,
 * or 0 on failure (0 is never a valid data block — block 0 is always
 * reserved/boot, even for 1024-byte-block volumes where first_data_block
 * is 1). */
static uint32_t alloc_block(ext2_fs_t *fs) {
    for (uint32_t group = 0; group < fs->groups_count; group++) {
        uint32_t bgd_blk, bgd_off;
        locate_bgd(fs, group, &bgd_blk, &bgd_off);
        if (read_block(fs, bgd_blk, meta_buf) != 0) return 0;

        ext2_group_desc_t bgd;
        memcpy(&bgd, &meta_buf[bgd_off], sizeof(bgd));
        if (bgd.bg_free_blocks_count == 0) continue;

        if (read_block(fs, bgd.bg_block_bitmap, bitmap_buf) != 0) return 0;

        uint32_t group_start = fs->first_data_block + group * fs->blocks_per_group;
        uint32_t group_end   = group_start + fs->blocks_per_group;
        if (group_end > fs->blocks_count) group_end = fs->blocks_count;
        uint32_t count = group_end - group_start;

        for (uint32_t bit = 0; bit < count; bit++) {
            uint32_t byte = bit / 8, off = bit % 8;
            if (bitmap_buf[byte] & (1u << off)) continue;

            bitmap_buf[byte] |= (uint8_t)(1u << off);
            if (write_block(fs, bgd.bg_block_bitmap, bitmap_buf) != 0) return 0;

            bgd.bg_free_blocks_count--;
            memcpy(&meta_buf[bgd_off], &bgd, sizeof(bgd));
            if (write_block(fs, bgd_blk, meta_buf) != 0) return 0;

            sb_adjust_free(fs, -1, 0);
            return group_start + bit;
        }
    }
    return 0;   /* volume full */
}

/* Like alloc_block, but tries one specific candidate block first — typically
 * "one past whatever physical block the previous logical block in this file
 * landed on", passed in by get_or_alloc_block — before falling back to
 * alloc_block's plain from-group-0 scan. That fallback scan always returns
 * the lowest-numbered free block on the whole volume, which means a file
 * growing one block at a time can get yanked backward to fill some
 * unrelated hole left by an earlier delete instead of continuing forward
 * from where it just was — every new block potentially a fresh seek away
 * from the last one. Trying the immediate next block first (an O(1) single-
 * bit check, not a rescan) is what keeps a sequentially-growing file
 * physically contiguous whenever the block right after it happens to be
 * free, without changing alloc_block's own fallback behavior at all when it
 * isn't. goal == 0 (no previous block yet — e.g. the file's very first
 * block) skips straight to that fallback. */
static uint32_t alloc_block_near(ext2_fs_t *fs, uint32_t goal) {
    if (goal >= fs->first_data_block && goal < fs->blocks_count) {
        uint32_t group = (goal - fs->first_data_block) / fs->blocks_per_group;
        uint32_t bit   = (goal - fs->first_data_block) % fs->blocks_per_group;

        uint32_t bgd_blk, bgd_off;
        locate_bgd(fs, group, &bgd_blk, &bgd_off);

        if (read_block(fs, bgd_blk, meta_buf) == 0) {
            ext2_group_desc_t bgd;
            memcpy(&bgd, &meta_buf[bgd_off], sizeof(bgd));

            if (bgd.bg_free_blocks_count > 0 &&
                read_block(fs, bgd.bg_block_bitmap, bitmap_buf) == 0) {
                uint32_t byte = bit / 8, off = bit % 8;

                if (!(bitmap_buf[byte] & (1u << off))) {
                    bitmap_buf[byte] |= (uint8_t)(1u << off);
                    if (write_block(fs, bgd.bg_block_bitmap, bitmap_buf) == 0) {
                        bgd.bg_free_blocks_count--;
                        memcpy(&meta_buf[bgd_off], &bgd, sizeof(bgd));
                        if (write_block(fs, bgd_blk, meta_buf) == 0) {
                            sb_adjust_free(fs, -1, 0);
                            return goal;
                        }
                    }
                }
            }
        }
    }

    return alloc_block(fs);
}

/* Find and reserve the longest contiguous run of free blocks available, up
 * to `want` long — used only by ext2_defrag_one, which (unlike ordinary
 * growth through get_or_alloc_block/alloc_block_near) needs a guaranteed
 * single run to move a whole file's data into at once, not just a good
 * next block. Mirrors fs/fat32.c: fat32_alloc_run's longest-run-with-early-
 * exit search, adapted to ext2's per-group bitmap instead of one flat
 * cluster numbering: a run never spans two groups (each group's bitmap is
 * a separate on-disk block, and groups are large in practice — e.g. 8192
 * blocks per group for a 1KB-block volume — so this never actually
 * constrains a real file). Groups are tried in order, first one offering a
 * full-length run wins immediately; otherwise the best (longest) run seen
 * across every group is used, which may be shorter than `want`.
 *
 * Returns the run's first physical block (0 if not even one free block
 * exists anywhere) and sets *out_len to how many it actually got
 * (1..want). Already reserved (bitmap flipped, counters updated) by the
 * time this returns, same as alloc_block/alloc_block_near. */
static uint32_t alloc_block_run(ext2_fs_t *fs, uint32_t want, uint32_t *out_len) {
    if (want == 0) want = 1;

    for (uint32_t group = 0; group < fs->groups_count; group++) {
        uint32_t bgd_blk, bgd_off;
        locate_bgd(fs, group, &bgd_blk, &bgd_off);
        if (read_block(fs, bgd_blk, meta_buf) != 0) return 0;

        ext2_group_desc_t bgd;
        memcpy(&bgd, &meta_buf[bgd_off], sizeof(bgd));
        if (bgd.bg_free_blocks_count == 0) continue;

        if (read_block(fs, bgd.bg_block_bitmap, bitmap_buf) != 0) return 0;

        uint32_t group_start = fs->first_data_block + group * fs->blocks_per_group;
        uint32_t group_end   = group_start + fs->blocks_per_group;
        if (group_end > fs->blocks_count) group_end = fs->blocks_count;
        uint32_t count = group_end - group_start;

        uint32_t run_start = 0, run_len = 0;
        uint32_t best_start = 0, best_len = 0;

        for (uint32_t bit = 0; bit < count; bit++) {
            uint32_t byte = bit / 8, off = bit % 8;
            if (!(bitmap_buf[byte] & (1u << off))) {
                if (run_len == 0) run_start = bit;
                run_len++;
                if (run_len > best_len) { best_len = run_len; best_start = run_start; }
                if (best_len >= want) break;
            } else {
                run_len = 0;
            }
        }

        if (best_len == 0) continue;   /* group's free count > 0 but scan found nothing? try the next one */
        if (best_len > want) best_len = want;

        for (uint32_t i = 0; i < best_len; i++) {
            uint32_t bit = best_start + i;
            uint32_t byte = bit / 8, off = bit % 8;
            bitmap_buf[byte] |= (uint8_t)(1u << off);
        }
        if (write_block(fs, bgd.bg_block_bitmap, bitmap_buf) != 0) return 0;

        bgd.bg_free_blocks_count -= (uint16_t)best_len;
        memcpy(&meta_buf[bgd_off], &bgd, sizeof(bgd));
        if (write_block(fs, bgd_blk, meta_buf) != 0) return 0;

        sb_adjust_free(fs, -(int32_t)best_len, 0);

        *out_len = best_len;
        return group_start + best_start;
    }

    return 0;   /* no group has a single free block, let alone a run */
}

/* Find and reserve one free inode. Returns the inode number (1-based), or 0
 * on failure. Skips every inode below fs->first_ino — those are reserved
 * (root=2, bad-blocks=1, journal=8, ...) regardless of what the bitmap
 * itself says, same rule every real ext2/3/4 implementation follows. */
static uint32_t alloc_inode(ext2_fs_t *fs) {
    for (uint32_t group = 0; group < fs->groups_count; group++) {
        uint32_t bgd_blk, bgd_off;
        locate_bgd(fs, group, &bgd_blk, &bgd_off);
        if (read_block(fs, bgd_blk, meta_buf) != 0) return 0;

        ext2_group_desc_t bgd;
        memcpy(&bgd, &meta_buf[bgd_off], sizeof(bgd));
        if (bgd.bg_free_inodes_count == 0) continue;

        if (read_block(fs, bgd.bg_inode_bitmap, bitmap_buf) != 0) return 0;

        uint32_t group_start_ino = group * fs->inodes_per_group;   /* 0-based */
        uint32_t count = fs->inodes_per_group;
        uint32_t remain = fs->inodes_count - group_start_ino;
        if (remain < count) count = remain;

        for (uint32_t bit = 0; bit < count; bit++) {
            uint32_t ino = group_start_ino + bit + 1;
            if (ino < fs->first_ino) continue;

            uint32_t byte = bit / 8, off = bit % 8;
            if (bitmap_buf[byte] & (1u << off)) continue;

            bitmap_buf[byte] |= (uint8_t)(1u << off);
            if (write_block(fs, bgd.bg_inode_bitmap, bitmap_buf) != 0) return 0;

            bgd.bg_free_inodes_count--;
            memcpy(&meta_buf[bgd_off], &bgd, sizeof(bgd));
            if (write_block(fs, bgd_blk, meta_buf) != 0) return 0;

            sb_adjust_free(fs, 0, -1);
            return ino;
        }
    }
    return 0;   /* no free inodes */
}

/* Clear one block's bit — the mirror-image of alloc_block. `block` must be
 * a block this driver itself allocated (or an existing direct/single-
 * indirect data block being freed by ext2_unlink); no bounds validation
 * beyond what alloc_block's own math already guarantees for such blocks. */
static int free_block(ext2_fs_t *fs, uint32_t block) {
    uint32_t group = (block - fs->first_data_block) / fs->blocks_per_group;
    uint32_t bit   = (block - fs->first_data_block) % fs->blocks_per_group;

    uint32_t bgd_blk, bgd_off;
    locate_bgd(fs, group, &bgd_blk, &bgd_off);
    if (read_block(fs, bgd_blk, meta_buf) != 0) return -1;

    ext2_group_desc_t bgd;
    memcpy(&bgd, &meta_buf[bgd_off], sizeof(bgd));

    if (read_block(fs, bgd.bg_block_bitmap, bitmap_buf) != 0) return -1;
    uint32_t byte = bit / 8, off = bit % 8;
    bitmap_buf[byte] &= (uint8_t)~(1u << off);
    if (write_block(fs, bgd.bg_block_bitmap, bitmap_buf) != 0) return -1;

    bgd.bg_free_blocks_count++;
    memcpy(&meta_buf[bgd_off], &bgd, sizeof(bgd));
    if (write_block(fs, bgd_blk, meta_buf) != 0) return -1;

    sb_adjust_free(fs, 1, 0);
    return 0;
}

/* Clear one inode's bit — the mirror-image of alloc_inode. */
static int free_inode(ext2_fs_t *fs, uint32_t ino) {
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t bit   = (ino - 1) % fs->inodes_per_group;

    uint32_t bgd_blk, bgd_off;
    locate_bgd(fs, group, &bgd_blk, &bgd_off);
    if (read_block(fs, bgd_blk, meta_buf) != 0) return -1;

    ext2_group_desc_t bgd;
    memcpy(&bgd, &meta_buf[bgd_off], sizeof(bgd));

    if (read_block(fs, bgd.bg_inode_bitmap, bitmap_buf) != 0) return -1;
    uint32_t byte = bit / 8, off = bit % 8;
    bitmap_buf[byte] &= (uint8_t)~(1u << off);
    if (write_block(fs, bgd.bg_inode_bitmap, bitmap_buf) != 0) return -1;

    bgd.bg_free_inodes_count++;
    memcpy(&meta_buf[bgd_off], &bgd, sizeof(bgd));
    if (write_block(fs, bgd_blk, meta_buf) != 0) return -1;

    sb_adjust_free(fs, 0, 1);
    return 0;
}

/* Walk the ext4 extent tree from `eh` down to the leaf containing `logical`.
 * Index nodes are read into ind_buf; entries within a node are sorted by
 * first logical block, so we descend into the last entry <= logical. */
static uint32_t ext4_extent_lookup(ext2_fs_t *fs, ext4_extent_header_t *eh,
                                   uint32_t logical) {
    for (int level = 0; level < 8; level++) {   /* depth is tiny in practice */
        if (eh->eh_magic != EXT4_EXT_MAGIC) return 0;

        if (eh->eh_depth == 0) {
            ext4_extent_t *ext = (ext4_extent_t *)(eh + 1);
            for (uint16_t i = 0; i < eh->eh_entries; i++) {
                uint16_t len = ext[i].ee_len;
                if (len > 32768) continue;   /* unwritten extent — hole */
                if (logical >= ext[i].ee_block &&
                    logical <  ext[i].ee_block + len)
                    return ext[i].ee_start_lo + (logical - ext[i].ee_block);
            }
            return 0;   /* hole */
        }

        /* Index node: pick the last entry whose ei_block <= logical */
        ext4_extent_idx_t *idx = (ext4_extent_idx_t *)(eh + 1);
        if (eh->eh_entries == 0 || logical < idx[0].ei_block) return 0;
        uint16_t k = 0;
        while (k + 1 < eh->eh_entries && idx[k + 1].ei_block <= logical)
            k++;

        if (read_block(fs, idx[k].ei_leaf_lo, ind_buf) != 0)
            return 0;
        eh = (ext4_extent_header_t *)ind_buf;
    }
    return 0;
}

/* Resolve a logical block number to a physical block number.
 * Handles direct, single/double/triple indirect blocks and ext4 extents. */
static uint32_t get_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical) {
    uint32_t ptrs = fs->block_size / 4;

    /* ── ext4 extents ── */
    if (inode->i_flags & EXT4_EXTENTS_FL)
        return ext4_extent_lookup(fs,
                                  (ext4_extent_header_t *)inode->i_block,
                                  logical);

    /* ── Direct blocks (0-11) ── */
    if (logical < 12)
        return inode->i_block[logical];

    /* ── Single indirect (12 .. 12+ptrs-1) ── */
    logical -= 12;
    if (logical < ptrs) {
        if (inode->i_block[12] == 0) return 0;
        if (read_block(fs, inode->i_block[12], ind_buf) != 0)
            return 0;
        return ((uint32_t *)ind_buf)[logical];
    }

    /* ── Double indirect ── */
    logical -= ptrs;
    if (logical < ptrs * ptrs) {
        if (inode->i_block[13] == 0) return 0;
        if (read_block(fs, inode->i_block[13], ind_buf) != 0)
            return 0;
        uint32_t ind2 = ((uint32_t *)ind_buf)[logical / ptrs];
        if (ind2 == 0) return 0;
        if (read_block(fs, ind2, ind_buf) != 0)
            return 0;
        return ((uint32_t *)ind_buf)[logical % ptrs];
    }

    /* ── Triple indirect ── */
    logical -= ptrs * ptrs;
    if (logical / ptrs / ptrs < ptrs) {
        if (inode->i_block[14] == 0) return 0;
        if (read_block(fs, inode->i_block[14], ind_buf) != 0)
            return 0;
        uint32_t ind2 = ((uint32_t *)ind_buf)[logical / (ptrs * ptrs)];
        if (ind2 == 0) return 0;
        if (read_block(fs, ind2, ind_buf) != 0)
            return 0;
        uint32_t ind3 = ((uint32_t *)ind_buf)[(logical / ptrs) % ptrs];
        if (ind3 == 0) return 0;
        if (read_block(fs, ind3, ind_buf) != 0)
            return 0;
        return ((uint32_t *)ind_buf)[logical % ptrs];
    }

    return 0;   /* beyond triple indirect range */
}

/* Resolve a logical block number to a physical one, ALLOCATING it (and, for
 * the single-indirect range, the indirect pointer block itself) if it isn't
 * already there. `inode` is mutated in place (i_block[]/i_blocks) — the
 * caller is responsible for write_inode()'ing it back afterward, same
 * "batch the writes, don't flush every step" split fat32_write uses.
 *
 * Deliberately only covers direct blocks (0-11) and single indirect
 * (12..12+ptrs-1) — double/triple indirect growth isn't implemented (this
 * driver's scope is ext2/3 without the ext4 extent-tree rewrite either;
 * growing a file/directory past ~4MB-16MB depending on block size just
 * isn't supported yet). Returns 0 if the target is beyond that range, the
 * volume is full, or `inode` uses ext4 extents (EXT4_EXTENTS_FL) — extent
 * growth is a separate, much larger undertaking, not attempted here. */
static uint32_t get_or_alloc_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical) {
    if (inode->i_flags & EXT4_EXTENTS_FL) return 0;

    uint32_t ptrs = fs->block_size / 4;

    if (logical < 12) {
        if (inode->i_block[logical] == 0) {
            /* Goal = one past the previous direct block, if this file has
             * one — keeps a file growing block-by-block physically
             * contiguous instead of alloc_block's plain scan jumping to
             * whatever the lowest-numbered free block on the volume is.
             * See alloc_block_near's own doc comment. */
            uint32_t goal = (logical > 0 && inode->i_block[logical - 1] != 0)
                             ? inode->i_block[logical - 1] + 1 : 0;
            uint32_t nb = alloc_block_near(fs, goal);
            if (!nb) return 0;
            inode->i_block[logical] = nb;
            inode->i_blocks += fs->sectors_per_block;
        }
        return inode->i_block[logical];
    }

    logical -= 12;
    if (logical < ptrs) {
        if (inode->i_block[12] == 0) {
            uint32_t nb = alloc_block(fs);   /* the indirect block itself: no useful goal yet */
            if (!nb) return 0;
            memset(ind_buf, 0, fs->block_size);
            if (write_block(fs, nb, ind_buf) != 0) return 0;
            inode->i_block[12] = nb;
            inode->i_blocks += fs->sectors_per_block;
        }

        if (read_block(fs, inode->i_block[12], ind_buf) != 0) return 0;
        uint32_t *ptr_arr = (uint32_t *)ind_buf;

        if (ptr_arr[logical] == 0) {
            /* Same goal idea, bridging the direct/indirect boundary too:
             * the previous logical block is either ptr_arr[logical-1]
             * (still inside the indirect range) or, for the very first
             * indirect entry, i_block[11] (the last direct block). */
            uint32_t goal = 0;
            if (logical > 0 && ptr_arr[logical - 1] != 0)
                goal = ptr_arr[logical - 1] + 1;
            else if (logical == 0 && inode->i_block[11] != 0)
                goal = inode->i_block[11] + 1;

            uint32_t nb = alloc_block_near(fs, goal);
            if (!nb) return 0;
            ptr_arr[logical] = nb;
            if (write_block(fs, inode->i_block[12], ind_buf) != 0) return 0;
            inode->i_blocks += fs->sectors_per_block;
        }
        return ptr_arr[logical];
    }

    return 0;   /* double/triple indirect growth: not supported */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_readdir
 * ══════════════════════════════════════════════════════════════════════════ */

dirent_t *ext2_readdir(vfs_node_t *node, uint32_t index) {
    ext2_fs_t *fs = (ext2_fs_t *)node->fs_private;
    if (!fs) return 0;

    ext2_inode_t dir_inode;
    if (read_inode(fs, node->inode, &dir_inode) != 0)
        return 0;

    uint32_t dir_size = dir_inode.i_size;
    uint32_t pos = 0;
    uint32_t valid_count = 0;
    uint32_t cur_logical = 0xFFFFFFFF;

    while (pos < dir_size) {
        uint32_t blk_log = pos / fs->block_size;
        uint32_t blk_off = pos % fs->block_size;

        if (blk_log != cur_logical) {
            uint32_t phys = get_block(fs, &dir_inode, blk_log);
            if (phys == 0) return 0;
            if (read_block(fs, phys, block_buf) != 0)
                return 0;
            cur_logical = blk_log;
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)&block_buf[blk_off];
        if (de->rec_len == 0) return 0;

        if (de->inode != 0 && de->name_len > 0) {
            char *name = (char *)(de + 1);
            /* Skip . and .. */
            int skip = 0;
            if (de->name_len == 1 && name[0] == '.') skip = 1;
            if (de->name_len == 2 && name[0] == '.' && name[1] == '.') skip = 1;

            if (!skip) {
                if (valid_count == index) {
                    int len = de->name_len;
                    if (len >= MAX_FILENAME) len = MAX_FILENAME - 1;
                    for (int i = 0; i < len; i++)
                        readdir_result.name[i] = name[i];
                    readdir_result.name[len] = '\0';

                    readdir_result.type = (de->file_type == EXT2_FT_DIR)
                                            ? VFS_DIRECTORY : VFS_FILE;

                    /* Read child inode for size (uses meta_buf, not block_buf) */
                    ext2_inode_t child;
                    if (read_inode(fs, de->inode, &child) == 0)
                        readdir_result.size = child.i_size;
                    else
                        readdir_result.size = 0;

                    return &readdir_result;
                }
                valid_count++;
            }
        }

        pos += de->rec_len;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_finddir
 * ══════════════════════════════════════════════════════════════════════════ */

vfs_node_t *ext2_finddir(vfs_node_t *node, const char *name) {
    ext2_fs_t *fs = (ext2_fs_t *)node->fs_private;
    if (!fs) return 0;

    int name_len = 0;
    while (name[name_len]) name_len++;

    ext2_inode_t dir_inode;
    if (read_inode(fs, node->inode, &dir_inode) != 0)
        return 0;

    uint32_t dir_size = dir_inode.i_size;
    uint32_t pos = 0;
    uint32_t cur_logical = 0xFFFFFFFF;

    while (pos < dir_size) {
        uint32_t blk_log = pos / fs->block_size;
        uint32_t blk_off = pos % fs->block_size;

        if (blk_log != cur_logical) {
            uint32_t phys = get_block(fs, &dir_inode, blk_log);
            if (phys == 0) return 0;
            if (read_block(fs, phys, block_buf) != 0)
                return 0;
            cur_logical = blk_log;
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)&block_buf[blk_off];
        if (de->rec_len == 0) return 0;

        if (de->inode != 0 && de->name_len == (uint8_t)name_len) {
            char *dname = (char *)(de + 1);
            int match = 1;
            for (int i = 0; i < name_len; i++) {
                if (dname[i] != name[i]) { match = 0; break; }
            }
            if (match) {
                ext2_inode_t child;
                if (read_inode(fs, de->inode, &child) != 0)
                    return 0;

                vfs_node_t *found = alloc_node();
                if (!found) return 0;

                int nl = name_len < MAX_FILENAME - 1 ? name_len : MAX_FILENAME - 1;
                for (int i = 0; i < nl; i++)
                    found->name[i] = name[i];
                found->name[nl] = '\0';

                found->inode      = de->inode;
                found->size       = child.i_size;
                found->fs_private = fs;
                found->parent     = node;

                if (child.i_mode & EXT2_S_IFDIR) {
                    found->type    = VFS_DIRECTORY;
                    found->readdir = ext2_readdir;
                    found->finddir = ext2_finddir;
                    found->create  = ext2_create;
                    found->unlink  = ext2_unlink;
                    found->rename  = ext2_rename;
                } else {
                    found->type  = VFS_FILE;
                    found->read  = ext2_read;
                    found->write = ext2_write;
                }

                return found;
            }
        }

        pos += de->rec_len;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_read — read bytes from a file
 * ══════════════════════════════════════════════════════════════════════════ */

int ext2_read(vfs_node_t *node, uint64_t offset, uint32_t size, void *buffer) {
    ext2_fs_t *fs = (ext2_fs_t *)node->fs_private;
    if (!fs) return -1;

    ext2_inode_t inode;
    if (read_inode(fs, node->inode, &inode) != 0)
        return -1;

    uint32_t file_size = inode.i_size;
    if (offset >= file_size) return 0;
    if (offset + size > file_size)
        size = file_size - offset;

    uint8_t *out = (uint8_t *)buffer;
    uint32_t bytes_read = 0;

    while (bytes_read < size) {
        uint32_t cur   = offset + bytes_read;
        uint32_t blk_l = cur / fs->block_size;
        uint32_t blk_o = cur % fs->block_size;

        uint32_t phys = get_block(fs, &inode, blk_l);
        if (phys == 0) return (int)bytes_read;

        if (read_block(fs, phys, block_buf) != 0)
            return -1;

        uint32_t copy = fs->block_size - blk_o;
        if (copy > size - bytes_read)
            copy = size - bytes_read;

        memcpy(&out[bytes_read], &block_buf[blk_o], (int)copy);
        bytes_read += copy;
    }

    return (int)bytes_read;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_write — write bytes to a file, growing it as needed
 *
 *  Two phases, same split fs/fat32.c: fat32_write uses: first make sure
 *  every logical block the write touches is allocated (get_or_alloc_block,
 *  clamping the write short if the volume fills up or the write would
 *  reach past what get_or_alloc_block can grow — see its own doc comment),
 *  then a plain read-modify-write loop copying bytes in, one ext2 block at
 *  a time (no cross-block batching the way fat32_write batches whole
 *  clusters — ext2 blocks aren't grouped into a coarser allocation unit
 *  the way FAT clusters are, so there's no equivalent run to batch).
 *
 *  Like fat32_write, a write has to start at or before the current EOF
 *  (offset <= inode.i_size) — no sparse "hole" writes.
 *
 *  Returns number of bytes actually written, or negative on error.
 * ══════════════════════════════════════════════════════════════════════════ */

int ext2_write(vfs_node_t *node, uint64_t offset, uint32_t size, const void *buffer) {
    ext2_fs_t *fs = (ext2_fs_t *)node->fs_private;
    if (!fs) return -1;
    if (!fs->write_supported) return -1;   /* see ext2_mount: unsafe ro_compat feature or dirty state */
    if (!(node->type & VFS_FILE)) return -1;
    if (size == 0) return 0;

    ext2_inode_t inode;
    if (read_inode(fs, node->inode, &inode) != 0) return -1;
    if (inode.i_flags & EXT4_EXTENTS_FL) return -1;   /* extent growth: unsupported */
    if (inode.i_size_high != 0) return -1;   /* file already >4GB — 32-bit i_size math below can't represent it */
    if (offset > inode.i_size) return -1;             /* sparse holes: unsupported */

    uint64_t new_end   = offset + size;
    uint32_t first_blk = (uint32_t)(offset / fs->block_size);
    uint32_t last_blk  = (uint32_t)((new_end - 1) / fs->block_size);

    /* Phase 1: ensure every logical block in [first_blk, last_blk] exists. */
    uint32_t got_blocks = 0;
    for (uint32_t lb = first_blk; lb <= last_blk; lb++) {
        if (get_or_alloc_block(fs, &inode, lb) == 0) break;
        got_blocks++;
    }

    uint64_t allocated_end = (uint64_t)(first_blk + got_blocks) * fs->block_size;
    if (new_end > allocated_end) {
        new_end = allocated_end;
        size = (new_end > offset) ? (uint32_t)(new_end - offset) : 0;
    }
    if (size == 0) return 0;

    /* Phase 2: read-modify-write loop, block by block. */
    const uint8_t *in = (const uint8_t *)buffer;
    uint32_t bytes_written = 0;

    while (bytes_written < size) {
        uint64_t cur   = offset + bytes_written;
        uint32_t blk_l = (uint32_t)(cur / fs->block_size);
        uint32_t blk_o = (uint32_t)(cur % fs->block_size);

        uint32_t phys = get_block(fs, &inode, blk_l);   /* allocated in phase 1 */
        if (phys == 0) break;

        if (read_block(fs, phys, block_buf) != 0) break;

        uint32_t copy = fs->block_size - blk_o;
        if (copy > size - bytes_written)
            copy = size - bytes_written;

        memcpy(&block_buf[blk_o], &in[bytes_written], (int)copy);

        if (write_block(fs, phys, block_buf) != 0) break;
        bytes_written += copy;
    }

    if (bytes_written == 0) return -1;

    if (offset + bytes_written > inode.i_size)
        inode.i_size = (uint32_t)(offset + bytes_written);
    if (write_inode(fs, node->inode, &inode) != 0) return -1;

    node->size = inode.i_size;
    return (int)bytes_written;
}

/* Round up to the next multiple of 4 — ext2 directory entries (and the
 * "used" prefix of one, name included) must be 4-byte aligned. */
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

/* Common tail of ext2_create's two paths: a fresh inode number is on disk
 * in the directory already (or about to be), give it real inode content
 * and wrap it in a vfs_node_t. */
static vfs_node_t *finish_new_file_node(ext2_fs_t *fs, vfs_node_t *dir, uint32_t ino,
                                        const char *name, int name_len) {
    ext2_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode        = EXT2_S_IFREG | 0644;   /* rw-r--r--; no permissions model here */
    new_inode.i_links_count = 1;
    if (write_inode(fs, ino, &new_inode) != 0) return 0;

    vfs_node_t *node = alloc_node();
    if (!node) return 0;

    int nl = name_len < MAX_FILENAME - 1 ? name_len : MAX_FILENAME - 1;
    for (int i = 0; i < nl; i++) node->name[i] = name[i];
    node->name[nl] = '\0';

    node->inode      = ino;
    node->size       = 0;
    node->type       = VFS_FILE;
    node->read       = ext2_read;
    node->write      = ext2_write;
    node->fs_private = fs;
    node->parent     = dir;

    return node;
}

/* Finds (or makes room for, appending a new block if nothing fits) a spot
 * in `dir`'s entries for a new directory entry pointing at `ino`, and
 * writes it. Shared by ext2_create (a freshly alloc_inode()'d inode) and
 * ext2_rename's cross-directory move / same-dir-rename-with-no-slack (an
 * EXISTING inode — the file's data doesn't move, just which entry names
 * it).
 *
 * ext2 directory entries are variable-length (rec_len), not fixed slots
 * like FAT32's — so "find a free spot" means one of two things:
 *
 *  1. An existing LIVE entry (de->inode != 0) whose rec_len is bigger than
 *     its own real content needs — very common, since mkfs and most real
 *     ext2 drivers pad the last entry in a block out to the block's end
 *     rather than leaving a separate free record. Shrink that entry's
 *     rec_len down to what it actually needs and place the new entry in
 *     the leftover space right after it.
 *  2. An already-free record (de->inode == 0, left behind by ext2_unlink
 *     or an ext2_rename that moved something out). Reused whole, keeping
 *     its rec_len as-is rather than splitting further — simpler, at the
 *     cost of not reclaiming excess slack.
 *
 * If nothing in the existing chain has room, appends one new block to the
 * directory (get_or_alloc_block — same growth path file writes use) and
 * fills it with just the new entry, rec_len spanning the whole block.
 *
 * Returns 0 on success, -1 (bad name length, not a directory, volume full,
 * I/O error). Note: leaves `block_buf` holding whatever block it last
 * touched — any caller still holding a pointer INTO block_buf from before
 * this call (e.g. ext2_rename's target_de) must re-read and re-locate
 * afterward rather than trust it. */
static int ext2_insert_dirent(ext2_fs_t *fs, vfs_node_t *dir, const char *name,
                              int name_len, uint32_t ino, uint8_t file_type) {
    if (name_len <= 0 || name_len > 255) return -1;
    uint32_t new_min = align4(8 + (uint32_t)name_len);

    ext2_inode_t dir_inode;
    if (read_inode(fs, dir->inode, &dir_inode) != 0) return -1;

    uint32_t dir_size    = dir_inode.i_size;
    uint32_t pos         = 0;
    uint32_t cur_logical = 0xFFFFFFFF;
    uint32_t cur_phys    = 0;

    while (pos < dir_size) {
        uint32_t blk_log = pos / fs->block_size;
        uint32_t blk_off = pos % fs->block_size;

        if (blk_log != cur_logical) {
            uint32_t phys = get_block(fs, &dir_inode, blk_log);
            if (phys == 0) break;
            if (read_block(fs, phys, block_buf) != 0) break;
            cur_logical = blk_log;
            cur_phys    = phys;
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)&block_buf[blk_off];
        if (de->rec_len == 0) break;   /* malformed — fall through to append-new-block */

        uint32_t used  = (de->inode != 0) ? align4(8 + de->name_len) : 0;
        uint32_t avail = de->rec_len - used;

        if (avail >= new_min) {
            ext2_dir_entry_t *new_de;
            if (used == 0) {
                new_de = de;   /* fully-free record — reuse as-is, keep its rec_len */
            } else {
                uint16_t old_rec_len = de->rec_len;
                de->rec_len = (uint16_t)used;
                new_de = (ext2_dir_entry_t *)((uint8_t *)de + used);
                new_de->rec_len = (uint16_t)(old_rec_len - used);
            }

            new_de->inode     = ino;
            new_de->name_len  = (uint8_t)name_len;
            new_de->file_type = file_type;
            memcpy((uint8_t *)(new_de + 1), name, (uint32_t)name_len);

            return write_block(fs, cur_phys, block_buf);
        }

        pos += de->rec_len;
    }

    /* No slack anywhere in the existing directory — append one new block
     * holding just this one entry, spanning the whole block (the same
     * convention mkfs itself uses for a directory's last live entry). */
    uint32_t new_logical = dir_size / fs->block_size;
    uint32_t new_phys = get_or_alloc_block(fs, &dir_inode, new_logical);
    if (!new_phys) return -1;

    memset(block_buf, 0, fs->block_size);
    ext2_dir_entry_t *de = (ext2_dir_entry_t *)block_buf;
    de->inode     = ino;
    de->rec_len   = (uint16_t)fs->block_size;
    de->name_len  = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy((uint8_t *)(de + 1), name, (uint32_t)name_len);
    if (write_block(fs, new_phys, block_buf) != 0) return -1;

    dir_inode.i_size += fs->block_size;
    return write_inode(fs, dir->inode, &dir_inode);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_create — create a new, empty file inside a directory
 *
 *  Returns the new vfs_node_t, or NULL if `dir` isn't a directory, `name`
 *  is empty or longer than ext2's 1-byte name_len can hold (255), or the
 *  volume is full (no free inode/block).
 * ══════════════════════════════════════════════════════════════════════════ */

vfs_node_t *ext2_create(vfs_node_t *dir, const char *name) {
    ext2_fs_t *fs = (ext2_fs_t *)dir->fs_private;
    if (!fs) return 0;
    if (!fs->write_supported) return 0;   /* see ext2_mount: unsafe ro_compat feature or dirty state */
    if (!(dir->type & VFS_DIRECTORY)) return 0;

    int name_len = 0;
    while (name[name_len]) name_len++;
    if (name_len == 0 || name_len > 255) return 0;

    uint32_t new_ino = alloc_inode(fs);
    if (!new_ino) return 0;

    if (ext2_insert_dirent(fs, dir, name, name_len, new_ino, EXT2_FT_REG_FILE) != 0) {
        free_inode(fs, new_ino);
        return 0;
    }

    return finish_new_file_node(fs, dir, new_ino, name, name_len);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_rename — move/rename a file within the same volume
 *
 *  Same directory, new name fits the existing entry's rec_len (the common
 *  case — see ext2_insert_dirent's doc comment on why entries usually have
 *  slack): rewrite name_len/name in place, cheapest possible path.
 *
 *  Otherwise (cross-directory move, or a same-directory rename whose new
 *  name doesn't fit the old slot): ext2_insert_dirent adds a new entry —
 *  in new_dir, pointing at the SAME inode number, no alloc_inode — then
 *  the old entry is cleared (inode=0, leaving it as a reusable free record
 *  for a future ext2_create/ext2_insert_dirent) WITHOUT freeing any inode
 *  or data blocks, unlike ext2_unlink. i_links_count is deliberately left
 *  untouched throughout: removing one link and adding one back is a net
 *  zero change, whether or not the two entries land in the same directory.
 *
 *  Only regular files — no rmdir semantics, same limit as ext2_unlink.
 * ══════════════════════════════════════════════════════════════════════════ */

int ext2_rename(vfs_node_t *old_dir, const char *old_name,
                vfs_node_t *new_dir, const char *new_name) {
    ext2_fs_t *fs = (ext2_fs_t *)old_dir->fs_private;
    if (!fs) return -1;
    if (!fs->write_supported) return -1;
    if (!(old_dir->type & VFS_DIRECTORY) || !(new_dir->type & VFS_DIRECTORY)) return -1;

    int old_len = 0; while (old_name[old_len]) old_len++;
    int new_len = 0; while (new_name[new_len]) new_len++;
    if (old_len == 0 || new_len == 0 || new_len > 255) return -1;

    ext2_inode_t old_dir_inode;
    if (read_inode(fs, old_dir->inode, &old_dir_inode) != 0) return -1;

    uint32_t dir_size     = old_dir_inode.i_size;
    uint32_t pos          = 0;
    uint32_t cur_logical  = 0xFFFFFFFF;
    uint32_t cur_phys     = 0;
    uint32_t target_ino   = 0;
    uint8_t  target_ftype = 0;
    uint32_t target_off   = 0;   /* byte offset of the entry within cur_phys */
    uint16_t target_rec_len = 0;

    while (pos < dir_size) {
        uint32_t blk_log = pos / fs->block_size;
        uint32_t blk_off = pos % fs->block_size;

        if (blk_log != cur_logical) {
            uint32_t phys = get_block(fs, &old_dir_inode, blk_log);
            if (phys == 0) return -2;
            if (read_block(fs, phys, block_buf) != 0) return -2;
            cur_logical = blk_log;
            cur_phys    = phys;
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)&block_buf[blk_off];
        if (de->rec_len == 0) break;

        if (de->inode != 0 && de->name_len == (uint8_t)old_len) {
            char *dname = (char *)(de + 1);
            int match = 1;
            for (int i = 0; i < old_len; i++)
                if (dname[i] != old_name[i]) { match = 0; break; }
            if (match) {
                target_ino      = de->inode;
                target_ftype    = de->file_type;
                target_off      = blk_off;
                target_rec_len  = de->rec_len;
                break;
            }
        }

        pos += de->rec_len;
    }

    if (!target_ino) return -3;   /* source not found */

    ext2_inode_t target_inode;
    if (read_inode(fs, target_ino, &target_inode) != 0) return -4;
    if (target_inode.i_mode & EXT2_S_IFDIR) return -5;

    if (old_dir == new_dir) {
        uint32_t new_min = align4(8 + (uint32_t)new_len);
        if (new_min <= target_rec_len) {
            /* block_buf still holds cur_phys from the scan above — the
             * common case needs nothing more than rewriting this one
             * entry's name in place. */
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)&block_buf[target_off];
            de->name_len = (uint8_t)new_len;
            memcpy((uint8_t *)(de + 1), new_name, (uint32_t)new_len);
            return write_block(fs, cur_phys, block_buf) == 0 ? 0 : -6;
        }
        /* Doesn't fit the existing slot — fall through to the general
         * insert-new/detach-old path below, same one a cross-directory
         * move needs anyway. */
    }

    /* ext2_insert_dirent freely reads/writes the shared block_buf for
     * WHATEVER block it ends up touching (possibly cur_phys itself, if the
     * new entry happens to land in the same directory) — block_buf can no
     * longer be trusted to hold cur_phys's contents after this call, so
     * the old entry gets cleared via a fresh read below rather than
     * reusing any pointer computed before this point. */
    if (ext2_insert_dirent(fs, new_dir, new_name, new_len, target_ino, target_ftype) != 0)
        return -7;

    if (read_block(fs, cur_phys, block_buf) != 0) return -8;
    ext2_dir_entry_t *old_de = (ext2_dir_entry_t *)&block_buf[target_off];
    old_de->inode = 0;
    if (write_block(fs, cur_phys, block_buf) != 0) return -8;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_unlink — remove a file
 *
 *  Finds the target's directory entry by a manual scan (the same scan
 *  ext2_finddir/ext2_create already do), clears its inode field to 0 —
 *  leaving name/rec_len alone turns it into exactly the "already-free
 *  record" ext2_create's slot search knows how to reuse whole, so this and
 *  ext2_create stay in sync without either needing to know about the
 *  other's internals — then, if this was the file's last link, frees its
 *  inode and data blocks.
 *
 *  Deliberately refuses (rather than leaking blocks it doesn't understand)
 *  three cases: directories (no rmdir semantics — nothing here checks
 *  emptiness or recurses), ext4 extent-based files (EXT4_EXTENTS_FL — this
 *  driver has no extent-tree code at all, write or free), and any file
 *  with double/triple indirect blocks in use (i_block[13]/[14] — the same
 *  boundary fs/ext2.c: get_or_alloc_block draws for growth; freeing only
 *  what's understood while skipping those would orphan real data blocks
 *  with nothing left pointing at them once the inode is gone).
 *
 *  Returns 0 on success, negative on error (not found, refused, or an I/O
 *  failure partway through — in which case the directory entry has already
 *  been cleared, so the file becomes unreachable/leaked rather than a
 *  dangling name over inconsistent data; a real fsck would reclaim it).
 * ══════════════════════════════════════════════════════════════════════════ */

int ext2_unlink(vfs_node_t *dir, const char *name) {
    ext2_fs_t *fs = (ext2_fs_t *)dir->fs_private;
    if (!fs) return -1;
    if (!fs->write_supported) return -1;
    if (!(dir->type & VFS_DIRECTORY)) return -1;

    int name_len = 0;
    while (name[name_len]) name_len++;
    if (name_len == 0) return -1;

    ext2_inode_t dir_inode;
    if (read_inode(fs, dir->inode, &dir_inode) != 0) return -1;

    uint32_t dir_size    = dir_inode.i_size;
    uint32_t pos          = 0;
    uint32_t cur_logical  = 0xFFFFFFFF;
    uint32_t cur_phys     = 0;
    uint32_t target_ino   = 0;
    ext2_dir_entry_t *target_de = 0;

    while (pos < dir_size) {
        uint32_t blk_log = pos / fs->block_size;
        uint32_t blk_off = pos % fs->block_size;

        if (blk_log != cur_logical) {
            uint32_t phys = get_block(fs, &dir_inode, blk_log);
            if (phys == 0) return -2;
            if (read_block(fs, phys, block_buf) != 0) return -2;
            cur_logical = blk_log;
            cur_phys    = phys;
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)&block_buf[blk_off];
        if (de->rec_len == 0) break;

        if (de->inode != 0 && de->name_len == (uint8_t)name_len) {
            char *dname = (char *)(de + 1);
            int match = 1;
            for (int i = 0; i < name_len; i++)
                if (dname[i] != name[i]) { match = 0; break; }
            if (match) { target_ino = de->inode; target_de = de; break; }
        }

        pos += de->rec_len;
    }

    if (!target_ino) return -3;   /* not found */

    ext2_inode_t target_inode;
    if (read_inode(fs, target_ino, &target_inode) != 0) return -4;

    if (target_inode.i_mode & EXT2_S_IFDIR) return -5;
    if (target_inode.i_flags & EXT4_EXTENTS_FL) return -6;
    if (target_inode.i_block[13] != 0 || target_inode.i_block[14] != 0) return -7;

    target_de->inode = 0;
    if (write_block(fs, cur_phys, block_buf) != 0) return -8;

    if (target_inode.i_links_count > 1) {
        target_inode.i_links_count--;
        write_inode(fs, target_ino, &target_inode);
        return 0;   /* other names still reference this inode */
    }

    for (int i = 0; i < 12; i++)
        if (target_inode.i_block[i] != 0)
            free_block(fs, target_inode.i_block[i]);

    if (target_inode.i_block[12] != 0) {
        if (read_block(fs, target_inode.i_block[12], ind_buf) == 0) {
            uint32_t *ptrs = (uint32_t *)ind_buf;
            uint32_t n = fs->block_size / 4;
            for (uint32_t i = 0; i < n; i++)
                if (ptrs[i] != 0) free_block(fs, ptrs[i]);
        }
        free_block(fs, target_inode.i_block[12]);
    }

    free_inode(fs, target_ino);

    /* Clear the inode's own on-disk record, not just its bitmap bit.
     * e2fsck's pass 1 treats i_links_count (not the bitmap) as the
     * authoritative "is this inode in use" signal — leaving a stale
     * i_links_count=1 with dangling i_block[] pointers here means a LATER
     * alloc_inode() reusing some OTHER, still-free inode number would look
     * fine on its own, but this now-orphaned record still claims the same
     * physical blocks a subsequent alloc_block() legitimately hands out to
     * someone else — e2fsck reports that as "multiply-claimed blocks" and
     * an "unattached inode". Caught by running a real e2fsck after this
     * exact create+move+copy+unlink sequence, not by inspection. */
    ext2_inode_t cleared;
    memset(&cleared, 0, sizeof(cleared));
    write_inode(fs, target_ino, &cleared);

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_defrag / ext2_defrag_one — relocate a fragmented file's data blocks
 *  into a single contiguous run
 *
 *  Same motivation and safety ordering as fs/fat32.c: fat32_defrag (see its
 *  doc comment) — alloc_block_run finds a single run big enough for every
 *  data block the file has, every block gets COPIED to the new run while
 *  the inode still points entirely at the old one (so a failure here
 *  leaves the file completely untouched, just frees the unused new run),
 *  and only once every byte is safely at the new location does the inode
 *  actually get repointed at it. Only then are the old blocks freed.
 *
 *  One structural difference from FAT32 worth calling out: ext2 files are
 *  addressed by inode number, not by a dirent somewhere in a parent
 *  directory, so there's no separate "repoint the directory entry" step at
 *  all — write_inode(fs, node->inode, &inode) IS the move, in one write.
 *
 *  Scope, matching get_or_alloc_block's own growth limit: only direct
 *  blocks (0-11) and single indirect are handled — ext4 extents
 *  (EXT4_EXTENTS_FL) and double/triple indirect files are left alone
 *  entirely (skipped, not an error), same as ext2_unlink already does for
 *  extent-based files rather than risk leaking or mishandling blocks in a
 *  layout this driver doesn't fully model. The single indirect pointer
 *  block itself is NOT relocated — only its contents (which data blocks it
 *  points to) are rewritten — so a file's data ends up fully contiguous,
 *  just with the one metadata block potentially sitting somewhere else on
 *  the volume; deliberately simpler and lower-risk than also moving it.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Returns 1 if `node` was moved, 0 if its data was already one contiguous
 * run (or empty — nothing to do), -1 if it couldn't be moved (extents/
 * double-triple-indirect/a hole, no single free run big enough, or an I/O
 * error) — either way the file is left exactly as it was. */
static int ext2_defrag_one(ext2_fs_t *fs, vfs_node_t *node) {
    ext2_inode_t inode;
    if (read_inode(fs, node->inode, &inode) != 0) return -1;
    if (inode.i_flags & EXT4_EXTENTS_FL) return -1;   /* not modeled here */
    if (inode.i_size == 0) return 0;                   /* empty file */

    uint32_t ptrs      = fs->block_size / 4;
    uint32_t max_blocks = 12 + ptrs;   /* direct + single indirect only */
    uint32_t count = (inode.i_size + fs->block_size - 1) / fs->block_size;
    if (count > max_blocks) return -1;   /* would need double/triple indirect */

    /* Collect the existing physical block list in logical order — also
     * doubles as the contiguity check and the hole check (a hole would
     * mean get_block returns 0, which this driver's own write path never
     * produces, but a file grown by something else before ending up here
     * could in principle have one; refuse rather than guess). */
    if (max_blocks > 12 + 1024) return -1;   /* defensive, shouldn't happen */
    uint32_t *old_blocks = old_blocks_buf;

    int already_ok = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys = get_block(fs, &inode, i);
        if (phys == 0) return -1;   /* hole — refuse rather than guess */
        old_blocks[i] = phys;
        if (i > 0 && phys != old_blocks[i - 1] + 1) already_ok = 0;
    }
    if (already_ok) return 0;

    uint32_t got = 0;
    uint32_t new_start = alloc_block_run(fs, count, &got);
    if (!new_start || got != count) {
        for (uint32_t i = 0; i < got; i++)
            free_block(fs, new_start + i);
        return -1;   /* no single run big enough right now */
    }

    /* Copy every block's data to the new run. Any failure here: free the
     * new run, bail — old_blocks[] (and the inode/indirect block that
     * reference them) haven't been touched at all yet. */
    for (uint32_t i = 0; i < count; i++) {
        if (read_block(fs, old_blocks[i], block_buf) != 0 ||
            write_block(fs, new_start + i, block_buf) != 0) {
            for (uint32_t j = 0; j < count; j++)
                free_block(fs, new_start + j);
            return -1;
        }
    }

    /* Commit: repoint the direct entries and write the inode back first —
     * this alone already makes logical blocks 0-11 correct at their new
     * location, regardless of what happens next. */
    uint32_t direct_count = count < 12 ? count : 12;
    for (uint32_t i = 0; i < direct_count; i++)
        inode.i_block[i] = new_start + i;

    if (write_inode(fs, node->inode, &inode) != 0) {
        for (uint32_t j = 0; j < count; j++)
            free_block(fs, new_start + j);
        return -1;   /* nothing committed on disk, old blocks still valid */
    }

    /* If there's an indirect range too, rewrite the SAME (old) indirect
     * block's contents to point into the new run's continuation — its own
     * physical location doesn't change, only what it points to. */
    if (count > 12) {
        if (read_block(fs, inode.i_block[12], ind_buf) != 0)
            return 1;   /* direct part already committed and correct; the
                          * indirect range stays on its old (still fully
                          * valid) blocks — partially defragmented, not
                          * corrupted, still worth reporting as "moved" */
        uint32_t *ptr_arr = (uint32_t *)ind_buf;
        for (uint32_t i = 12; i < count; i++)
            ptr_arr[i - 12] = new_start + i;
        if (write_block(fs, inode.i_block[12], ind_buf) != 0)
            return 1;   /* same partial-success case as above */
    }

    /* Every logical block now resolves to the new run — free the old
     * blocks (all of them, direct and indirect-range alike; the indirect
     * pointer block itself was never one of these, it just got repointed
     * above, not replaced). */
    for (uint32_t i = 0; i < count; i++)
        free_block(fs, old_blocks[i]);

    return 1;
}

int ext2_defrag(vfs_node_t *dir_node, uint32_t *out_scanned,
                uint32_t *out_moved, uint32_t *out_skipped) {
    ext2_fs_t *fs = (ext2_fs_t *)dir_node->fs_private;
    if (!fs) return -1;

    uint32_t scanned = 0, moved = 0, skipped = 0;

    for (uint32_t idx = 0; ; idx++) {
        dirent_t *de = ext2_readdir(dir_node, idx);
        if (!de) break;
        if (de->type & VFS_DIRECTORY) continue;   /* not recursive, files only */

        char name[MAX_FILENAME];
        int i = 0;
        while (de->name[i] && i < MAX_FILENAME - 1) { name[i] = de->name[i]; i++; }
        name[i] = '\0';

        vfs_node_t *node = ext2_finddir(dir_node, name);
        if (!node) continue;

        scanned++;
        int rc = ext2_defrag_one(fs, node);
        if (rc == 1) moved++;
        else if (rc < 0) skipped++;
    }

    if (out_scanned) *out_scanned = scanned;
    if (out_moved)   *out_moved   = moved;
    if (out_skipped) *out_skipped = skipped;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_mount — mount an ext2/3/4 partition
 * ══════════════════════════════════════════════════════════════════════════ */

int ext2_mount(uint8_t drive_type, uint8_t drive, uint64_t part_lba, mount_point_t *mp) {
    /* Superblock starts at byte 1024 = LBA+2 (two 512-byte sectors) */
    uint8_t sb_raw[1024];
    if (blk_read(drive_type, drive, part_lba + 2, 2, sb_raw) != 0)
        return -1;

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_raw;

    if (sb->s_magic != EXT2_MAGIC)
        return -2;

    /* Refuse the mount outright — not even read-only — if the volume uses
     * any incompat feature this driver doesn't understand (unreplayed
     * ext3/4 journal, 64-bit group descriptors, meta_bg, inline_data, ...).
     * See EXT2_SUPPORTED_INCOMPAT's doc comment: those bits change how the
     * on-disk layout must be interpreted, so mounting anyway risks silently
     * misreading (or, once write is involved, corrupting) the volume.
     * Only meaningful for EXT2_DYNAMIC_REV (rev_level >= 1) — the feature-
     * bits fields don't exist on old-rev volumes, which predate them and
     * are rare enough in practice not to worry about here. */
    if (sb->s_rev_level >= 1 && (sb->s_feature_incompat & ~EXT2_SUPPORTED_INCOMPAT))
        return -6;

    ext2_fs_t *fs = alloc_fs();
    if (!fs) return -3;

    fs->drive            = drive;
    fs->drive_type       = drive_type;
    fs->part_lba         = part_lba;
    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->sectors_per_block = fs->block_size / 512;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inode_size       = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->first_data_block = sb->s_first_data_block;
    fs->groups_count     = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                         / sb->s_blocks_per_group;
    fs->blocks_count     = sb->s_blocks_count;
    fs->inodes_count     = sb->s_inodes_count;
    fs->first_ino        = (sb->s_rev_level >= 1) ? sb->s_first_ino : 11;

    /* Write is gated separately (and more strictly) than the mount itself:
     * an unrecognized ro_compat bit (most importantly metadata_csum — every
     * bitmap/inode/dirent block carries a checksum this driver can't
     * compute) is safe to mount and READ, but a write would leave that
     * bookkeeping stale in a way a real fsck (or a live kernel) would flag
     * as corruption. s_state's EXT2_VALID_FS bit is checked too: if unset,
     * this volume either wasn't cleanly unmounted last time or IS mounted
     * elsewhere right now — writing on top of either is asking for trouble,
     * independent of which feature bits are set. */
    int state_clean  = sb->s_rev_level < 1 || (sb->s_state & EXT2_VALID_FS) != 0;
    int rocompat_ok  = sb->s_rev_level < 1 ||
                       (sb->s_feature_ro_compat & ~EXT2_SUPPORTED_ROCOMPAT_FOR_WRITE) == 0;
    fs->write_supported = (uint8_t)(state_clean && rocompat_ok);

    /* Root directory is always inode 2 */
    vfs_node_t *root = alloc_node();
    if (!root) return -4;

    root->name[0]    = '/';
    root->name[1]    = '\0';
    root->type       = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root->inode      = EXT2_ROOT_INO;
    root->size       = 0;
    root->fs_private = fs;
    root->parent     = 0;
    root->readdir    = ext2_readdir;
    root->finddir    = ext2_finddir;
    root->create     = ext2_create;
    root->unlink     = ext2_unlink;
    root->rename     = ext2_rename;

    mp->root     = root;
    mp->drive    = drive;
    mp->fs_type  = FS_EXT2;
    mp->part_lba = part_lba;
    mp->active   = 1;

    return 0;
}
