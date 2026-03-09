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
    uint32_t lba = fs->part_lba + block * fs->sectors_per_block;
    return ata_read_sectors(fs->drive, lba,
                            (uint8_t)fs->sectors_per_block, buf);
}

/* Read an inode by number.  Uses meta_buf internally. */
static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    /* Locate block group descriptor */
    uint32_t bgdt_block = fs->first_data_block + 1;
    uint32_t bgd_byte   = group * sizeof(ext2_group_desc_t);
    uint32_t bgd_blk    = bgdt_block + bgd_byte / fs->block_size;
    uint32_t bgd_off    = bgd_byte % fs->block_size;

    if (read_block(fs, bgd_blk, meta_buf) != 0)
        return -1;

    ext2_group_desc_t *bgd = (ext2_group_desc_t *)&meta_buf[bgd_off];
    uint32_t inode_table = bgd->bg_inode_table;

    /* Locate inode within the table */
    uint32_t inode_byte = index * fs->inode_size;
    uint32_t inode_blk  = inode_table + inode_byte / fs->block_size;
    uint32_t inode_off  = inode_byte % fs->block_size;

    if (read_block(fs, inode_blk, meta_buf) != 0)
        return -1;

    /* memcpy(source, dest, n) — CoolFrost non-standard order */
    memcpy(&meta_buf[inode_off], (uint8_t *)out, (int)sizeof(ext2_inode_t));
    return 0;
}

/* Resolve a logical block number to a physical block number.
 * Handles direct blocks, single indirect, and ext4 extents (depth 0). */
static uint32_t get_block(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical) {
    uint32_t ptrs = fs->block_size / 4;

    /* ── ext4 extents ── */
    if (inode->i_flags & EXT4_EXTENTS_FL) {
        ext4_extent_header_t *eh = (ext4_extent_header_t *)inode->i_block;
        if (eh->eh_magic != EXT4_EXT_MAGIC) return 0;
        if (eh->eh_depth == 0) {
            ext4_extent_t *ext = (ext4_extent_t *)(eh + 1);
            for (uint16_t i = 0; i < eh->eh_entries; i++) {
                if (logical >= ext[i].ee_block &&
                    logical <  ext[i].ee_block + ext[i].ee_len)
                    return ext[i].ee_start_lo + (logical - ext[i].ee_block);
            }
        }
        return 0;   /* depth > 0 not supported yet */
    }

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

    return 0;   /* triple indirect — not supported */
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
                } else {
                    found->type = VFS_FILE;
                    found->read = ext2_read;
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

int ext2_read(vfs_node_t *node, uint32_t offset, uint32_t size, void *buffer) {
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

        /* memcpy(source, dest, n) — CoolFrost non-standard order */
        memcpy(&block_buf[blk_o], &out[bytes_read], (int)copy);
        bytes_read += copy;
    }

    return (int)bytes_read;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ext2_mount — mount an ext2/3/4 partition (read-only)
 * ══════════════════════════════════════════════════════════════════════════ */

int ext2_mount(uint8_t drive, uint32_t part_lba, mount_point_t *mp) {
    /* Superblock starts at byte 1024 = LBA+2 (two 512-byte sectors) */
    uint8_t sb_raw[1024];
    if (ata_read_sectors(drive, part_lba + 2, 2, sb_raw) != 0)
        return -1;

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_raw;

    if (sb->s_magic != EXT2_MAGIC)
        return -2;

    ext2_fs_t *fs = alloc_fs();
    if (!fs) return -3;

    fs->drive            = drive;
    fs->part_lba         = part_lba;
    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->sectors_per_block = fs->block_size / 512;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inode_size       = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->first_data_block = sb->s_first_data_block;
    fs->groups_count     = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                         / sb->s_blocks_per_group;

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

    mp->root     = root;
    mp->drive    = drive;
    mp->fs_type  = FS_EXT2;
    mp->part_lba = part_lba;
    mp->active   = 1;

    return 0;
}
