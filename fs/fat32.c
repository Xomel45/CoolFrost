#include "fat32.h"
#include "../drivers/ata.h"
#include "../libc/mem.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Static pools — we have no reliable dynamic allocator, so use fixed arrays.
 * ══════════════════════════════════════════════════════════════════════════ */

#define MAX_FAT32_FS    4
#define MAX_VFS_NODES   128

static fat32_fs_t  fs_pool[MAX_FAT32_FS];
static uint8_t     fs_pool_used = 0;

static vfs_node_t  node_pool[MAX_VFS_NODES];
static uint8_t     node_pool_used = 0;

/* Separate buffers so readdir / read and FAT lookups don't clobber each other */
static uint8_t     sector_buf[512];   /* directory / file data      */
static uint8_t     fat_buf[512];      /* FAT table lookups          */

/* readdir returns a pointer to this static — caller must copy if needed */
static dirent_t    readdir_result;

/* ══════════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static vfs_node_t *alloc_node(void) {
    if (node_pool_used >= MAX_VFS_NODES) return 0;
    vfs_node_t *n = &node_pool[node_pool_used++];
    memset(n, 0, sizeof(vfs_node_t));
    return n;
}

static fat32_fs_t *alloc_fs(void) {
    if (fs_pool_used >= MAX_FAT32_FS) return 0;
    fat32_fs_t *f = &fs_pool[fs_pool_used++];
    memset(f, 0, sizeof(fat32_fs_t));
    return f;
}

/* Absolute LBA of the first sector of a data cluster */
static uint32_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    return fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

/* Follow the FAT chain: return the next cluster, or >= FAT32_EOC if end */
static uint32_t fat32_next_cluster(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = fs->fat_start_lba + (fat_offset / 512);
    uint32_t entry_off   = fat_offset % 512;

    if (ata_read_sectors(fs->drive, fat_sector, 1, fat_buf) != 0)
        return FAT32_EOC;

    uint32_t next = *(uint32_t *)&fat_buf[entry_off];
    return next & 0x0FFFFFFF;
}

/* Convert "HELLO   TXT" → "HELLO.TXT" */
static void fat32_format_83(const char *raw, char *out) {
    int j = 0;

    /* Base name (first 8 chars), trim trailing spaces */
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[j++] = raw[i];

    /* Extension (last 3 chars) */
    if (raw[8] != ' ') {
        out[j++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[j++] = raw[i];
    }

    out[j] = '\0';
}

/* Case-insensitive string compare (FAT stores names in uppercase) */
static int fat32_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Is this 32-byte directory entry one we should show to the user? */
static int fat32_entry_valid(fat32_dirent_t *de) {
    if ((uint8_t)de->name[0] == 0x00) return -1;   /* end of dir   */
    if ((uint8_t)de->name[0] == 0xE5) return 0;    /* deleted      */
    if (de->attr == FAT32_ATTR_LFN)   return 0;    /* LFN part     */
    if (de->attr & FAT32_ATTR_VOLUME_ID) return 0;  /* volume label */
    return 1;                                        /* valid entry  */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_readdir — return the Nth valid entry in a directory
 *
 *  node->inode  = first cluster of the directory
 *  node->fs_private = fat32_fs_t*
 *  Returns NULL when index is past the last entry.
 * ══════════════════════════════════════════════════════════════════════════ */

dirent_t *fat32_readdir(vfs_node_t *node, uint32_t index) {
    fat32_fs_t *fs = (fat32_fs_t *)node->fs_private;
    if (!fs) return 0;

    uint32_t cluster     = node->inode;
    uint32_t valid_count = 0;

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(fs, cluster);

        for (uint8_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (ata_read_sectors(fs->drive, lba + s, 1, sector_buf) != 0)
                return 0;

            fat32_dirent_t *entries = (fat32_dirent_t *)sector_buf;

            for (uint32_t e = 0; e < 512 / sizeof(fat32_dirent_t); e++) {
                int v = fat32_entry_valid(&entries[e]);
                if (v < 0) return 0;   /* end of directory */
                if (v == 0) continue;   /* skip this entry  */

                if (valid_count == index) {
                    fat32_format_83(entries[e].name, readdir_result.name);
                    readdir_result.size = entries[e].size;
                    readdir_result.type = (entries[e].attr & FAT32_ATTR_DIRECTORY)
                                            ? VFS_DIRECTORY : VFS_FILE;
                    return &readdir_result;
                }
                valid_count++;
            }
        }

        cluster = fat32_next_cluster(fs, cluster);
    }

    return 0;   /* index out of range */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_finddir — look up a name inside a directory, return a new vfs_node
 * ══════════════════════════════════════════════════════════════════════════ */

vfs_node_t *fat32_finddir(vfs_node_t *node, const char *name) {
    fat32_fs_t *fs = (fat32_fs_t *)node->fs_private;
    if (!fs) return 0;

    uint32_t cluster = node->inode;

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(fs, cluster);

        for (uint8_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (ata_read_sectors(fs->drive, lba + s, 1, sector_buf) != 0)
                return 0;

            fat32_dirent_t *entries = (fat32_dirent_t *)sector_buf;

            for (uint32_t e = 0; e < 512 / sizeof(fat32_dirent_t); e++) {
                int v = fat32_entry_valid(&entries[e]);
                if (v < 0) return 0;
                if (v == 0) continue;

                char formatted[MAX_FILENAME];
                fat32_format_83(entries[e].name, formatted);

                if (fat32_strcasecmp(formatted, name) != 0)
                    continue;

                /* ── Match found — allocate a vfs_node ── */
                vfs_node_t *found = alloc_node();
                if (!found) return 0;

                /* Copy name */
                int i;
                for (i = 0; formatted[i] && i < MAX_FILENAME - 1; i++)
                    found->name[i] = formatted[i];
                found->name[i] = '\0';

                found->size  = entries[e].size;
                found->inode = ((uint32_t)entries[e].cluster_high << 16)
                             |  (uint32_t)entries[e].cluster_low;
                found->fs_private = fs;
                found->parent     = node;

                if (entries[e].attr & FAT32_ATTR_DIRECTORY) {
                    found->type    = VFS_DIRECTORY;
                    found->readdir = fat32_readdir;
                    found->finddir = fat32_finddir;
                } else {
                    found->type = VFS_FILE;
                    found->read = fat32_read;
                }

                return found;
            }
        }

        cluster = fat32_next_cluster(fs, cluster);
    }

    return 0;   /* not found */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_read — read bytes from a file
 *
 *  node->inode = first cluster
 *  Returns number of bytes actually read, or negative on error.
 * ══════════════════════════════════════════════════════════════════════════ */

int fat32_read(vfs_node_t *node, uint32_t offset, uint32_t size, void *buffer) {
    fat32_fs_t *fs = (fat32_fs_t *)node->fs_private;
    if (!fs) return -1;
    if (!(node->type & VFS_FILE)) return -1;

    /* Clamp to file bounds */
    if (offset >= node->size) return 0;
    if (offset + size > node->size)
        size = node->size - offset;

    uint32_t cluster      = node->inode;
    uint32_t cluster_size = (uint32_t)fs->sectors_per_cluster * 512;
    uint32_t bytes_read   = 0;
    uint32_t pos          = 0;          /* byte offset at cluster start */

    /* Skip whole clusters before the offset */
    while (pos + cluster_size <= offset && cluster < FAT32_EOC) {
        pos += cluster_size;
        cluster = fat32_next_cluster(fs, cluster);
    }

    uint8_t *out = (uint8_t *)buffer;

    /* Read cluster by cluster */
    while (bytes_read < size && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(fs, cluster);

        for (uint8_t s = 0; s < fs->sectors_per_cluster && bytes_read < size; s++) {
            uint32_t sec_start = pos + (uint32_t)s * 512;
            uint32_t sec_end   = sec_start + 512;

            /* This sector is entirely before our read window — skip */
            if (sec_end <= offset)
                continue;

            if (ata_read_sectors(fs->drive, lba + s, 1, sector_buf) != 0)
                return -1;

            /* How much of this 512-byte sector do we need? */
            uint32_t copy_start = (offset > sec_start) ? (offset - sec_start) : 0;
            uint32_t copy_len   = 512 - copy_start;
            if (copy_len > size - bytes_read)
                copy_len = size - bytes_read;

            /* memcpy(source, dest, n) — CoolFrost non-standard order! */
            memcpy(&sector_buf[copy_start], &out[bytes_read], (int)copy_len);
            bytes_read += copy_len;
        }

        pos += cluster_size;
        cluster = fat32_next_cluster(fs, cluster);
    }

    return (int)bytes_read;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_mount — mount a FAT32 partition
 *
 *  Reads the BPB, validates it, computes layout, creates root vfs_node.
 *  Returns 0 on success.
 * ══════════════════════════════════════════════════════════════════════════ */

int fat32_mount(uint8_t drive, uint32_t part_lba, mount_point_t *mp) {
    /* Read boot sector */
    uint8_t boot[512];
    if (ata_read_sectors(drive, part_lba, 1, boot) != 0)
        return -1;

    fat32_bpb_t *bpb = (fat32_bpb_t *)boot;

    /* ── Validation ── */
    if (bpb->bytes_per_sector != 512)
        return -2;              /* only 512-byte sectors supported */
    if (bpb->root_entry_count != 0)
        return -3;              /* non-zero → FAT12/16, not FAT32  */
    if (bpb->fat_size_16 != 0)
        return -3;
    if (bpb->sectors_per_cluster == 0)
        return -3;

    /* ── Allocate internal state ── */
    fat32_fs_t *fs = alloc_fs();
    if (!fs) return -4;

    fs->drive               = drive;
    fs->part_lba            = part_lba;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->reserved_sectors    = bpb->reserved_sectors;
    fs->num_fats            = bpb->num_fats;
    fs->fat_size            = bpb->fat_size_32;
    fs->root_cluster        = bpb->root_cluster;
    fs->total_sectors       = bpb->total_sectors_32;

    /* Compute absolute LBAs */
    fs->fat_start_lba  = part_lba + bpb->reserved_sectors;
    fs->data_start_lba = fs->fat_start_lba
                       + (uint32_t)bpb->num_fats * bpb->fat_size_32;

    /* Copy & trim volume label */
    for (int i = 0; i < 11; i++)
        fs->volume_label[i] = bpb->volume_label[i];
    fs->volume_label[11] = '\0';
    for (int i = 10; i >= 0 && fs->volume_label[i] == ' '; i--)
        fs->volume_label[i] = '\0';

    /* ── Create root VFS node ── */
    vfs_node_t *root = alloc_node();
    if (!root) return -5;

    root->name[0]    = '/';
    root->name[1]    = '\0';
    root->type       = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root->inode      = fs->root_cluster;
    root->size       = 0;
    root->fs_private = fs;
    root->parent     = 0;
    root->readdir    = fat32_readdir;
    root->finddir    = fat32_finddir;

    /* ── Fill the mount point ── */
    mp->root         = root;
    mp->drive        = drive;
    mp->fs_type      = FS_FAT32;
    mp->part_lba     = part_lba;
    mp->active       = 1;

    return 0;
}
