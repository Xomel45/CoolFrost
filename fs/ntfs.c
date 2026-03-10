#include "ntfs.h"
#include "../drivers/ata.h"
#include "../libc/mem.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Static pools & buffers
 * ══════════════════════════════════════════════════════════════════════════ */

#define MAX_NTFS_FS     4
#define MAX_NTFS_NODES  128

static ntfs_fs_t   fs_pool[MAX_NTFS_FS];
static uint8_t     fs_pool_used = 0;

static vfs_node_t  node_pool[MAX_NTFS_NODES];
static uint8_t     node_pool_used = 0;

static uint8_t     mft_buf[4096];       /* MFT record (up to 4096)    */
static uint8_t     data_buf[4096];      /* cluster / INDX reads       */

static dirent_t    readdir_result;

/* ══════════════════════════════════════════════════════════════════════════
 *  Allocators
 * ══════════════════════════════════════════════════════════════════════════ */

static vfs_node_t *alloc_node(void) {
    if (node_pool_used >= MAX_NTFS_NODES) return 0;
    vfs_node_t *n = &node_pool[node_pool_used++];
    memset(n, 0, sizeof(vfs_node_t));
    return n;
}

static ntfs_fs_t *alloc_fs(void) {
    if (fs_pool_used >= MAX_NTFS_FS) return 0;
    ntfs_fs_t *f = &fs_pool[fs_pool_used++];
    memset(f, 0, sizeof(ntfs_fs_t));
    return f;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  UTF-16LE → ASCII helper
 * ══════════════════════════════════════════════════════════════════════════ */

static void utf16_to_ascii(const uint16_t *src, int len, char *dst, int max) {
    int j = 0;
    for (int i = 0; i < len && j < max - 1; i++) {
        if (src[i] < 0x80)
            dst[j++] = (char)src[i];
        else
            dst[j++] = '?';
    }
    dst[j] = '\0';
}

/* Simple ASCII case-insensitive compare */
static int ntfs_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  MFT record reading
 * ══════════════════════════════════════════════════════════════════════════ */

/* Apply fixup array to an MFT/INDX record */
static int apply_fixup(uint8_t *record, uint16_t fixup_offset,
                       uint16_t fixup_count, uint32_t record_size) {
    uint16_t *fixup = (uint16_t *)(record + fixup_offset);
    uint16_t usn = fixup[0];
    uint32_t sectors = record_size / 512;

    if (fixup_count < 2) return 0;
    for (uint32_t i = 1; i < fixup_count && i <= sectors; i++) {
        uint16_t *sector_end = (uint16_t *)(record + i * 512 - 2);
        if (*sector_end != usn) return -1;
        *sector_end = fixup[i];
    }
    return 0;
}

/* Read MFT record by number into mft_buf */
static int read_mft_record(ntfs_fs_t *fs, uint32_t record_num) {
    uint64_t lba = fs->mft_lba + (uint64_t)record_num * fs->mft_sectors;
    if (ata_read_sectors(fs->drive, lba,
                         (uint8_t)fs->mft_sectors, mft_buf) != 0)
        return -1;

    /* Validate signature */
    if (mft_buf[0] != 'F' || mft_buf[1] != 'I' ||
        mft_buf[2] != 'L' || mft_buf[3] != 'E')
        return -2;

    ntfs_mft_header_t *hdr = (ntfs_mft_header_t *)mft_buf;
    return apply_fixup(mft_buf, hdr->fixup_offset, hdr->fixup_count,
                       fs->mft_record_size);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Attribute searching
 * ══════════════════════════════════════════════════════════════════════════ */

/* Find first attribute of given type in mft_buf.  Returns NULL if not found. */
static ntfs_attr_common_t *find_attr(uint8_t *record, uint32_t type) {
    ntfs_mft_header_t *hdr = (ntfs_mft_header_t *)record;
    uint32_t off = hdr->first_attr_offset;

    while (off + sizeof(ntfs_attr_common_t) <= hdr->used_size) {
        ntfs_attr_common_t *attr = (ntfs_attr_common_t *)(record + off);
        if (attr->type == NTFS_ATTR_END) break;
        if (attr->length == 0) break;
        if (attr->type == type) return attr;
        off += attr->length;
    }
    return 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 *  Data run parsing
 * ══════════════════════════════════════════════════════════════════════════ */

/* Parse one data run entry.  Returns bytes consumed (0 = end of runs). */
static int parse_run(const uint8_t *run, uint32_t *length, int32_t *offset) {
    uint8_t header = run[0];
    if (header == 0) return 0;

    uint8_t len_sz = header & 0x0F;
    uint8_t off_sz = (header >> 4) & 0x0F;
    if (len_sz == 0 || len_sz > 4) return 0;
    if (off_sz > 4) return 0;

    /* Length (unsigned) */
    *length = 0;
    for (int i = 0; i < len_sz; i++)
        *length |= (uint32_t)run[1 + i] << (i * 8);

    /* Offset (signed, relative) */
    *offset = 0;
    for (int i = 0; i < off_sz; i++)
        *offset |= (int32_t)((uint32_t)run[1 + len_sz + i] << (i * 8));
    /* Sign-extend */
    if (off_sz > 0 && (run[len_sz + off_sz] & 0x80))
        for (int i = off_sz; i < 4; i++)
            *offset |= (int32_t)(0xFFu << (i * 8));

    return 1 + len_sz + off_sz;
}

/* Read `size` bytes at `offset` from a non-resident attribute into `buffer`.
 * Returns bytes read or negative on error. */
static int read_nonresident(ntfs_fs_t *fs, ntfs_attr_nonresident_t *attr,
                            uint32_t offset, uint32_t size, uint8_t *buffer) {
    const uint8_t *runs = (const uint8_t *)attr + attr->data_runs_offset;

    uint32_t bytes_read = 0;
    int32_t  prev_lcn = 0;
    uint32_t vcn_pos = 0;           /* byte position at start of current run */

    while (bytes_read < size) {
        uint32_t run_len;
        int32_t  run_off;
        int consumed = parse_run(runs, &run_len, &run_off);
        if (consumed == 0) break;
        runs += consumed;

        int32_t lcn = prev_lcn + run_off;
        prev_lcn = lcn;

        uint32_t run_bytes = run_len * fs->cluster_size;
        uint32_t run_end   = vcn_pos + run_bytes;

        /* Does this run overlap our read window? */
        uint32_t read_end = offset + size;
        if (run_end > offset && vcn_pos < read_end) {
            uint32_t copy_start = (offset > vcn_pos) ? offset : vcn_pos;
            uint32_t copy_end   = (read_end < run_end) ? read_end : run_end;

            uint32_t run_byte_off = copy_start - vcn_pos;
            uint32_t remaining   = copy_end - copy_start;

            while (remaining > 0) {
                uint32_t clust_idx    = run_byte_off / fs->cluster_size;
                uint32_t within_clust = run_byte_off % fs->cluster_size;
                uint64_t lba = fs->part_lba
                             + (uint64_t)(lcn + clust_idx) * fs->sectors_per_cluster;

                if (ata_read_sectors(fs->drive, lba,
                                     fs->sectors_per_cluster, data_buf) != 0)
                    return -1;

                uint32_t chunk = fs->cluster_size - within_clust;
                if (chunk > remaining) chunk = remaining;

                /* memcpy(source, dest, n) — CoolFrost order */
                memcpy(&data_buf[within_clust], &buffer[bytes_read], (int)chunk);
                bytes_read   += chunk;
                remaining    -= chunk;
                run_byte_off += chunk;
            }
        }

        vcn_pos = run_end;
    }

    return (int)bytes_read;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Index entry iteration helpers
 *
 *  Iterates index entries within an index node, calls `callback` for each.
 *  Returns the index of the entry that matched `target_index`, or -1.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Process index entries within an index node header region.
 * `base` points to the start of the index node header.
 * `counter` is the current valid-entry counter.
 * If `target_index` >= 0, stop when counter == target_index and fill `result`.
 * If `search_name` != NULL, stop when name matches and fill `result`.
 * Returns: updated counter, or -1 if a match was found.
 * On match, *match_mft and *match_fn are set. */
static int walk_index_entries(uint8_t *base, ntfs_index_node_header_t *node_hdr,
                              int counter, int target_index,
                              const char *search_name,
                              uint64_t *match_mft,
                              ntfs_filename_attr_t **match_fn) {
    uint32_t off = node_hdr->first_entry_offset;
    uint32_t end = node_hdr->total_size;

    while (off + sizeof(ntfs_index_entry_t) <= end) {
        ntfs_index_entry_t *ie = (ntfs_index_entry_t *)(base + off);

        if (ie->flags & 0x02) break;   /* last entry */
        if (ie->entry_length == 0) break;

        if (ie->content_length >= sizeof(ntfs_filename_attr_t)) {
            ntfs_filename_attr_t *fn = (ntfs_filename_attr_t *)
                ((uint8_t *)ie + sizeof(ntfs_index_entry_t));

            /* Skip DOS-only names */
            if (fn->name_namespace != NTFS_NS_DOS) {
                char ascii_name[MAX_FILENAME];
                uint16_t *uname = (uint16_t *)((uint8_t *)fn +
                                  sizeof(ntfs_filename_attr_t));
                utf16_to_ascii(uname, fn->name_length, ascii_name, MAX_FILENAME);

                /* Skip . and .. and system ($) entries */
                if (ascii_name[0] != '\0' && ascii_name[0] != '$') {
                    if (search_name) {
                        if (ntfs_strcasecmp(ascii_name, search_name) == 0) {
                            *match_mft = ie->mft_reference & 0x0000FFFFFFFFFFFF;
                            *match_fn  = fn;
                            return -1;
                        }
                    } else {
                        if (counter == target_index) {
                            *match_mft = ie->mft_reference & 0x0000FFFFFFFFFFFF;
                            *match_fn  = fn;
                            return -1;
                        }
                        counter++;
                    }
                }
            }
        }

        off += ie->entry_length;
    }

    return counter;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ntfs_readdir / ntfs_finddir — shared core
 *
 *  mode 0: readdir (find entry at `index`)
 *  mode 1: finddir (find entry with `name`)
 * ══════════════════════════════════════════════════════════════════════════ */

static int ntfs_dir_lookup(ntfs_fs_t *fs, uint32_t dir_mft,
                           int target_index, const char *search_name,
                           uint64_t *out_mft, ntfs_filename_attr_t **out_fn) {
    /* Read directory MFT record */
    if (read_mft_record(fs, dir_mft) != 0)
        return -1;

    /* ── $INDEX_ROOT (always resident) ── */
    ntfs_attr_common_t *ir_attr = find_attr(mft_buf, NTFS_ATTR_INDEX_ROOT);
    if (!ir_attr) return -1;

    ntfs_attr_resident_t *ir_res = (ntfs_attr_resident_t *)ir_attr;
    uint8_t *ir_value = (uint8_t *)ir_attr + ir_res->value_offset;

    /* Skip index root header (16 bytes) to reach the index node header */
    ntfs_index_node_header_t *node_hdr =
        (ntfs_index_node_header_t *)(ir_value + sizeof(ntfs_index_root_header_t));

    /* The entries are relative to node_hdr */
    int counter = walk_index_entries((uint8_t *)node_hdr, node_hdr,
                                     0, target_index, search_name,
                                     out_mft, out_fn);
    if (counter < 0) return 0;  /* found */

    /* ── $INDEX_ALLOCATION (non-resident, for larger directories) ── */
    /* Re-read MFT record since walk_index_entries might not have clobbered
     * mft_buf, but read_nonresident uses data_buf which is separate. */
    if (read_mft_record(fs, dir_mft) != 0)
        return -1;

    ntfs_attr_common_t *ia_attr = find_attr(mft_buf, NTFS_ATTR_INDEX_ALLOCATION);
    if (!ia_attr || !ia_attr->non_resident)
        return -1;  /* not found, no more entries */

    ntfs_attr_nonresident_t *ia_nr = (ntfs_attr_nonresident_t *)ia_attr;
    uint32_t total = (uint32_t)ia_nr->real_size;
    uint32_t rec_size = fs->index_record_size;
    if (rec_size == 0) rec_size = 4096;

    /* We need a separate buffer for INDX records so we don't clobber
     * data_buf which read_nonresident uses internally.
     * But since read_nonresident fills data_buf per cluster and we copy
     * to the target, we read INDX records in one go into a local buf.
     * For simplicity, we read one record at a time using read_nonresident
     * into data_buf, then copy and process. */

    /* Static buffer for one INDX record */
    static uint8_t indx_buf[4096];

    for (uint32_t off = 0; off + rec_size <= total; off += rec_size) {
        /* Read this INDX record via data runs */
        /* Re-read MFT record to re-parse data runs each iteration
         * (read_nonresident uses data_buf which we need for reading) */
        if (read_mft_record(fs, dir_mft) != 0)
            return -1;

        ia_attr = find_attr(mft_buf, NTFS_ATTR_INDEX_ALLOCATION);
        if (!ia_attr) return -1;
        ia_nr = (ntfs_attr_nonresident_t *)ia_attr;

        int rd = read_nonresident(fs, ia_nr, off, rec_size, indx_buf);
        if (rd < (int)rec_size) continue;

        /* Validate "INDX" signature */
        if (indx_buf[0] != 'I' || indx_buf[1] != 'N' ||
            indx_buf[2] != 'D' || indx_buf[3] != 'X')
            continue;

        /* Apply fixup */
        uint16_t fixup_off = *(uint16_t *)&indx_buf[4];
        uint16_t fixup_cnt = *(uint16_t *)&indx_buf[6];
        apply_fixup(indx_buf, fixup_off, fixup_cnt, rec_size);

        /* Index node header is at offset 24 in INDX record */
        ntfs_index_node_header_t *indx_node =
            (ntfs_index_node_header_t *)&indx_buf[24];

        counter = walk_index_entries(&indx_buf[24], indx_node,
                                     counter, target_index, search_name,
                                     out_mft, out_fn);
        if (counter < 0) return 0;  /* found */
    }

    return -1;  /* not found */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ntfs_readdir
 * ══════════════════════════════════════════════════════════════════════════ */

dirent_t *ntfs_readdir(vfs_node_t *node, uint32_t index) {
    ntfs_fs_t *fs = (ntfs_fs_t *)node->fs_private;
    if (!fs) return 0;

    uint64_t match_mft;
    ntfs_filename_attr_t *match_fn;

    if (ntfs_dir_lookup(fs, node->inode, (int)index, 0,
                        &match_mft, &match_fn) != 0)
        return 0;

    /* Extract name */
    uint16_t *uname = (uint16_t *)((uint8_t *)match_fn +
                      sizeof(ntfs_filename_attr_t));
    utf16_to_ascii(uname, match_fn->name_length,
                   readdir_result.name, MAX_FILENAME);

    readdir_result.size = (uint32_t)match_fn->real_size;
    readdir_result.type = (match_fn->flags & NTFS_FILE_ATTR_DIRECTORY)
                            ? VFS_DIRECTORY : VFS_FILE;

    return &readdir_result;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ntfs_finddir
 * ══════════════════════════════════════════════════════════════════════════ */

vfs_node_t *ntfs_finddir(vfs_node_t *node, const char *name) {
    ntfs_fs_t *fs = (ntfs_fs_t *)node->fs_private;
    if (!fs) return 0;

    uint64_t match_mft;
    ntfs_filename_attr_t *match_fn;

    if (ntfs_dir_lookup(fs, node->inode, -1, name,
                        &match_mft, &match_fn) != 0)
        return 0;

    vfs_node_t *found = alloc_node();
    if (!found) return 0;

    uint16_t *uname = (uint16_t *)((uint8_t *)match_fn +
                      sizeof(ntfs_filename_attr_t));
    utf16_to_ascii(uname, match_fn->name_length, found->name, MAX_FILENAME);

    found->inode      = (uint32_t)(match_mft & 0xFFFFFFFF);
    found->size       = (uint32_t)match_fn->real_size;
    found->fs_private = fs;
    found->parent     = node;

    if (match_fn->flags & NTFS_FILE_ATTR_DIRECTORY) {
        found->type    = VFS_DIRECTORY;
        found->readdir = ntfs_readdir;
        found->finddir = ntfs_finddir;
    } else {
        found->type = VFS_FILE;
        found->read = ntfs_read;
    }

    return found;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ntfs_read — read bytes from a file
 * ══════════════════════════════════════════════════════════════════════════ */

int ntfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, void *buffer) {
    ntfs_fs_t *fs = (ntfs_fs_t *)node->fs_private;
    if (!fs) return -1;

    if (read_mft_record(fs, node->inode) != 0)
        return -1;

    ntfs_attr_common_t *data_attr = find_attr(mft_buf, NTFS_ATTR_DATA);
    if (!data_attr) return -1;

    if (data_attr->non_resident == 0) {
        /* ── Resident data ── */
        ntfs_attr_resident_t *res = (ntfs_attr_resident_t *)data_attr;
        uint32_t data_len = res->value_length;
        uint8_t *data_ptr = (uint8_t *)data_attr + res->value_offset;

        if (offset >= data_len) return 0;
        if (offset + size > data_len)
            size = data_len - offset;

        /* memcpy(source, dest, n) — CoolFrost order */
        memcpy(&data_ptr[offset], (uint8_t *)buffer, (int)size);
        return (int)size;
    }

    /* ── Non-resident data ── */
    ntfs_attr_nonresident_t *nr = (ntfs_attr_nonresident_t *)data_attr;
    uint32_t file_size = (uint32_t)nr->real_size;

    if (offset >= file_size) return 0;
    if (offset + size > file_size)
        size = file_size - offset;

    return read_nonresident(fs, nr, offset, size, (uint8_t *)buffer);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ntfs_mount — mount an NTFS partition (read-only)
 * ══════════════════════════════════════════════════════════════════════════ */

int ntfs_mount(uint8_t drive, uint64_t part_lba, mount_point_t *mp) {
    uint8_t boot[512];
    if (ata_read_sectors(drive, part_lba, 1, boot) != 0)
        return -1;

    ntfs_boot_sector_t *bs = (ntfs_boot_sector_t *)boot;

    /* Validate OEM ID */
    if (bs->oem_id[0] != 'N' || bs->oem_id[1] != 'T' ||
        bs->oem_id[2] != 'F' || bs->oem_id[3] != 'S')
        return -2;

    if (bs->bytes_per_sector != 512)
        return -3;

    ntfs_fs_t *fs = alloc_fs();
    if (!fs) return -4;

    fs->drive               = drive;
    fs->part_lba            = part_lba;
    fs->sectors_per_cluster = bs->sectors_per_cluster;
    fs->cluster_size        = (uint32_t)bs->sectors_per_cluster * 512;

    /* MFT record size */
    if (bs->clusters_per_mft_record < 0)
        fs->mft_record_size = 1u << (uint32_t)(-(int)bs->clusters_per_mft_record);
    else
        fs->mft_record_size = (uint32_t)bs->clusters_per_mft_record * fs->cluster_size;

    fs->mft_sectors = fs->mft_record_size / 512;
    if (fs->mft_sectors == 0) fs->mft_sectors = 2;  /* minimum 1024 */

    /* Index record size */
    if (bs->clusters_per_index_record < 0)
        fs->index_record_size = 1u << (uint32_t)(-(int)bs->clusters_per_index_record);
    else
        fs->index_record_size = (uint32_t)bs->clusters_per_index_record * fs->cluster_size;
    if (fs->index_record_size == 0) fs->index_record_size = 4096;

    /* Absolute LBA of $MFT */
    fs->mft_lba = part_lba + bs->mft_cluster * (uint64_t)bs->sectors_per_cluster;

    /* Verify we can read MFT record 0 */
    if (read_mft_record(fs, 0) != 0)
        return -5;

    /* Create root VFS node (MFT record #5 = root directory) */
    vfs_node_t *root = alloc_node();
    if (!root) return -6;

    root->name[0]    = '/';
    root->name[1]    = '\0';
    root->type       = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root->inode      = NTFS_MFT_ROOT_DIR;
    root->size       = 0;
    root->fs_private = fs;
    root->parent     = 0;
    root->readdir    = ntfs_readdir;
    root->finddir    = ntfs_finddir;

    mp->root     = root;
    mp->drive    = drive;
    mp->fs_type  = FS_NTFS;
    mp->part_lba = part_lba;
    mp->active   = 1;

    return 0;
}
