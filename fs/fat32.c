#include "fat32.h"
#include "../drivers/ata.h"   /* blk_read, blk_write, DRIVE_TYPE_* */
#include "../libc/mem.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Static pools — we have no reliable dynamic allocator, so use fixed arrays.
 * ══════════════════════════════════════════════════════════════════════════ */

#define MAX_FAT32_FS    4
#define MAX_VFS_NODES   128

/* Upper bound on sectors/cluster this driver will handle — 128 sectors
 * (64KB) covers every FAT32-spec-valid cluster size (spec requires a power
 * of 2, max 64KB); fat32_mount() rejects anything larger so cluster_buf
 * below is always big enough by construction, with no fallback path to
 * carry (and not exercise) for an out-of-spec volume. */
#define FAT32_MAX_CLUSTER_SECTORS 128

static fat32_fs_t  fs_pool[MAX_FAT32_FS];
static uint8_t     fs_pool_used = 0;

static vfs_node_t  node_pool[MAX_VFS_NODES];
static uint8_t     node_pool_used = 0;

/* One fat32_file_info_t per vfs_node_t this driver hands out — allocated
 * in lockstep with node_pool (same mirrored-pool pattern as fs_pool /
 * node_pool above), never independently. */
static fat32_file_info_t finfo_pool[MAX_VFS_NODES];
static uint8_t           finfo_pool_used = 0;

/* Separate buffers so readdir / read-write and FAT lookups don't clobber
 * each other. cluster_buf holds fat32_read/fat32_write's per-cluster batch
 * transfer (see their doc comments) — sized for the worst case allowed by
 * FAT32_MAX_CLUSTER_SECTORS regardless of any one mounted volume's actual
 * (usually much smaller) cluster size. */
static uint8_t     sector_buf[512];                              /* directory entries */
static uint8_t     fat_buf[512];                                 /* FAT table lookups  */
static uint8_t     cluster_buf[FAT32_MAX_CLUSTER_SECTORS * 512];  /* file data batch    */

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

static fat32_file_info_t *alloc_finfo(void) {
    if (finfo_pool_used >= MAX_VFS_NODES) return 0;
    fat32_file_info_t *fi = &finfo_pool[finfo_pool_used++];
    memset(fi, 0, sizeof(fat32_file_info_t));
    return fi;
}

/* Absolute LBA of the first sector of a data cluster */
static uint64_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    return fs->data_start_lba + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

/* fat_buf's cache key — which (drive_type, drive, LBA) its contents belong
 * to, so fat32_next_cluster can skip the blk_read when the next lookup
 * lands in the same FAT sector (extremely common: consecutive clusters of
 * an unfragmented file/chain sit 4 bytes apart, 128 to a 512-byte FAT
 * sector, so a whole run of fat32_next_cluster calls hits one sector).
 * cached_lba starts at a value no real LBA can ever equal, so the first
 * lookup after boot is always a cold miss. No FAT-writing code exists yet
 * (fat32_write only overwrites existing file data, never touches the FAT
 * itself) — if that changes, whatever writes to the FAT must also
 * invalidate/update this cache, or a stale read could follow a chain link
 * that's since been overwritten on disk. */
static uint64_t cached_fat_lba        = (uint64_t)-1;
static uint8_t  cached_fat_drive_type = 0xFF;
static uint8_t  cached_fat_drive      = 0xFF;

/* Follow the FAT chain: return the next cluster, or >= FAT32_EOC if end */
static uint32_t fat32_next_cluster(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset  = cluster * 4;
    uint64_t fat_sector  = fs->fat_start_lba + (fat_offset / 512);
    uint32_t entry_off   = fat_offset % 512;

    if (fat_sector != cached_fat_lba || fs->drive_type != cached_fat_drive_type ||
        fs->drive != cached_fat_drive) {
        if (blk_read(fs->drive_type, fs->drive, fat_sector, 1, fat_buf) != 0)
            return FAT32_EOC;
        cached_fat_lba        = fat_sector;
        cached_fat_drive_type = fs->drive_type;
        cached_fat_drive      = fs->drive;
    }

    uint32_t next = *(uint32_t *)&fat_buf[entry_off];
    return next & 0x0FFFFFFF;
}

/* Write one FAT entry, to every FAT copy (fs->num_fats — real FAT32 volumes
 * keep 2 for redundancy, and both need to agree or a disk checker/other OS
 * would see a corrupt table). Top 4 bits of a FAT32 entry are reserved and
 * must survive a write untouched, so this does a read-modify-write per
 * copy rather than blindly overwriting all 32 bits. */
static int fat32_set_fat_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint64_t sector_idx = fat_offset / 512;
    uint32_t entry_off  = fat_offset % 512;
    uint8_t  buf[512];

    for (uint8_t copy = 0; copy < fs->num_fats; copy++) {
        uint64_t fat_sector = fs->fat_start_lba + (uint64_t)copy * fs->fat_size + sector_idx;
        if (blk_read(fs->drive_type, fs->drive, fat_sector, 1, buf) != 0)
            return -1;
        uint32_t old = *(uint32_t *)&buf[entry_off];
        *(uint32_t *)&buf[entry_off] = (value & 0x0FFFFFFF) | (old & 0xF0000000);
        if (blk_write(fs->drive_type, fs->drive, fat_sector, 1, buf) != 0)
            return -1;
    }

    /* fat32_next_cluster's read cache only ever holds copy 0 (it always
     * reads via fs->fat_start_lba directly) — keep it coherent so a lookup
     * right after this write doesn't see a stale cached sector. */
    uint64_t primary_sector = fs->fat_start_lba + sector_idx;
    if (primary_sector == cached_fat_lba && fs->drive_type == cached_fat_drive_type &&
        fs->drive == cached_fat_drive) {
        uint32_t old = *(uint32_t *)&fat_buf[entry_off];
        *(uint32_t *)&fat_buf[entry_off] = (value & 0x0FFFFFFF) | (old & 0xF0000000);
    }

    return 0;
}

/* Find one free cluster (FAT entry == 0), mark it FAT32_EOC, return its
 * number. Returns 0 on failure (disk full / I/O error) — 0 and 1 are never
 * valid data cluster numbers in FAT32 (numbering starts at 2), so 0 doubles
 * as a NULL-like sentinel. Searches from fs->next_free_hint forward, then
 * wraps to the start of the data area, so repeated calls while growing one
 * file don't re-scan clusters already known to be taken. */
static uint32_t fat32_alloc_cluster(fat32_fs_t *fs) {
    uint32_t max_cluster = fs->total_clusters + 1;   /* clusters start at 2 */
    uint32_t start = fs->next_free_hint;
    if (start < 2 || start > max_cluster) start = 2;

    for (uint32_t c = start; c <= max_cluster; c++) {
        if (fat32_next_cluster(fs, c) == 0) {
            if (fat32_set_fat_entry(fs, c, FAT32_EOC) != 0) return 0;
            fs->next_free_hint = c + 1;
            return c;
        }
    }
    for (uint32_t c = 2; c < start; c++) {
        if (fat32_next_cluster(fs, c) == 0) {
            if (fat32_set_fat_entry(fs, c, FAT32_EOC) != 0) return 0;
            fs->next_free_hint = c + 1;
            return c;
        }
    }

    return 0;   /* volume full */
}

/* Find and reserve the longest contiguous run of free clusters available,
 * up to `want` clusters long. Exists alongside fat32_alloc_cluster (not
 * built on top of it) to fix a real fragmentation pattern: repeatedly
 * calling the single-cluster allocator hands out whatever's nearest to
 * next_free_hint one at a time, which happily scatters a growing file
 * across small holes left by earlier deletes (e.g. clusters 2 and 4, with
 * 3 occupied by something unrelated) even when a large contiguous free
 * extent exists a bit further out on the volume. Searching for the best
 * available run BEFORE committing to any single cluster is what actually
 * avoids that.
 *
 * Same forward-from-hint-then-wrap search order as fat32_alloc_cluster,
 * but tracking the best (longest, capped at `want`) run of consecutive
 * free clusters seen instead of stopping at the first free one; stops
 * early only once a full-length run turns up. On success, the run's own
 * FAT entries are already chained together internally (run[i] ->
 * run[i+1], last -> FAT32_EOC as a temporary terminator, same convention
 * fat32_alloc_cluster uses) — the caller still has to link the file's
 * previous tail cluster onto run_start itself.
 *
 * Returns the run's first cluster (0 on failure — disk full/I/O error)
 * and sets *out_len to how many clusters it actually got (1..want; can be
 * less than requested if no bigger run exists, letting the caller fall
 * back to appending more runs instead of failing the whole write). */
static uint32_t fat32_alloc_run(fat32_fs_t *fs, uint32_t want, uint32_t *out_len) {
    if (want == 0) want = 1;
    uint32_t max_cluster = fs->total_clusters + 1;
    uint32_t start = fs->next_free_hint;
    if (start < 2 || start > max_cluster) start = 2;

    uint32_t best_start = 0, best_len = 0;

    for (int pass = 0; pass < 2 && best_len < want; pass++) {
        uint32_t lo = (pass == 0) ? start : 2;
        uint32_t hi = (pass == 0) ? max_cluster : (start - 1);
        if (lo > hi) continue;

        uint32_t run_start = 0, run_len = 0;

        for (uint32_t c = lo; c <= hi; c++) {
            if (fat32_next_cluster(fs, c) == 0) {
                if (run_len == 0) run_start = c;
                run_len++;
                if (run_len > best_len) { best_len = run_len; best_start = run_start; }
                if (best_len >= want) break;
            } else {
                run_len = 0;
            }
        }
    }

    if (best_len == 0) return 0;   /* volume full */
    if (best_len > want) best_len = want;   /* loop above already stops at ==, defensive */

    for (uint32_t i = 0; i < best_len; i++) {
        uint32_t cl   = best_start + i;
        uint32_t next = (i + 1 < best_len) ? (cl + 1) : FAT32_EOC;
        if (fat32_set_fat_entry(fs, cl, next) != 0) return 0;
    }

    fs->next_free_hint = best_start + best_len;
    *out_len = best_len;
    return best_start;
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

/* Convert "HELLO.TXT" → raw 11-byte 8.3 record ("HELLO   TXT", uppercased,
 * space-padded) — the inverse of fat32_format_83. Returns 0 if `name`
 * doesn't fit the format (base > 8 chars, extension > 3 chars, more than
 * one '.', or empty base) rather than truncating or guessing: this driver
 * has no LFN support anywhere (fat32_entry_valid skips LFN entries
 * outright), so a name it can't losslessly round-trip through 8.3 can't be
 * created here at all. */
static int fat32_parse_83(const char *name, char *raw) {
    for (int i = 0; i < 11; i++) raw[i] = ' ';

    int i = 0, j = 0;
    while (name[i] && name[i] != '.') {
        if (j >= 8) return 0;
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        raw[j++] = c;
        i++;
    }
    if (j == 0) return 0;   /* empty base name */

    if (name[i] == '.') {
        i++;
        int k = 0;
        while (name[i]) {
            if (name[i] == '.' || k >= 3) return 0;
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            raw[8 + k] = c;
            k++;
            i++;
        }
    }
    return 1;
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
 *  node->fs_private = fat32_file_info_t*
 *  Returns NULL when index is past the last entry.
 * ══════════════════════════════════════════════════════════════════════════ */

dirent_t *fat32_readdir(vfs_node_t *node, uint32_t index) {
    fat32_file_info_t *node_fi = (fat32_file_info_t *)node->fs_private;
    if (!node_fi) return 0;
    fat32_fs_t *fs = node_fi->fs;

    uint32_t cluster     = node->inode;
    uint32_t valid_count = 0;

    while (cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(fs, cluster);

        for (uint8_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (blk_read(fs->drive_type, fs->drive, lba + s, 1, sector_buf) != 0)
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
    fat32_file_info_t *node_fi = (fat32_file_info_t *)node->fs_private;
    if (!node_fi) return 0;
    fat32_fs_t *fs = node_fi->fs;

    uint32_t cluster = node->inode;

    while (cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(fs, cluster);

        for (uint8_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (blk_read(fs->drive_type, fs->drive, lba + s, 1, sector_buf) != 0)
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
                fat32_file_info_t *fi = alloc_finfo();
                if (!fi) return 0;

                /* Copy name */
                int i;
                for (i = 0; formatted[i] && i < MAX_FILENAME - 1; i++)
                    found->name[i] = formatted[i];
                found->name[i] = '\0';

                found->size  = entries[e].size;
                found->inode = ((uint32_t)entries[e].cluster_high << 16)
                             |  (uint32_t)entries[e].cluster_low;
                found->parent     = node;

                fi->fs         = fs;
                fi->has_dirent = 1;
                fi->dirent_lba = lba + s;
                fi->dirent_off = (uint16_t)(e * sizeof(fat32_dirent_t));
                found->fs_private = fi;

                if (entries[e].attr & FAT32_ATTR_DIRECTORY) {
                    found->type    = VFS_DIRECTORY;
                    found->readdir = fat32_readdir;
                    found->finddir = fat32_finddir;
                    found->create  = fat32_create;
                    found->unlink  = fat32_unlink;
                    found->rename  = fat32_rename;
                } else {
                    found->type  = VFS_FILE;
                    found->read  = fat32_read;
                    found->write = fat32_write;
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
 *
 *  Per cluster, does ONE multi-sector blk_read for the whole contiguous run
 *  of sectors that overlaps [offset, offset+size) instead of one blk_read
 *  per 512-byte sector — ata_read_sectors already transfers `count` sectors
 *  in a single PIO command (drivers/ata.c), so this cuts the number of disk
 *  commands from one per sector to one per cluster, which matters a lot on
 *  real FAT32 volumes (8-64 sectors/cluster is typical; our own tiny test
 *  hdd.img happens to format at 1 sector/cluster, so it won't show a
 *  difference there, but the win is real for anything realistically sized).
 *  The run is always contiguous because offset/size don't change mid-loop —
 *  every sector between the first and last that overlap the window does too.
 * ══════════════════════════════════════════════════════════════════════════ */

int fat32_read(vfs_node_t *node, uint64_t offset, uint32_t size, void *buffer) {
    fat32_file_info_t *fi = (fat32_file_info_t *)node->fs_private;
    if (!fi) return -1;
    fat32_fs_t *fs = fi->fs;
    if (!(node->type & VFS_FILE)) return -1;

    /* Clamp to file bounds */
    if (offset >= node->size) return 0;
    if (offset + size > node->size)
        size = node->size - offset;

    uint32_t cluster      = node->inode;
    uint32_t cluster_size = (uint32_t)fs->sectors_per_cluster * 512;
    uint32_t bytes_read   = 0;
    uint64_t pos          = 0;          /* byte offset at cluster start */

    /* Skip whole clusters before the offset */
    while (pos + cluster_size <= offset && cluster < FAT32_EOC) {
        pos += cluster_size;
        cluster = fat32_next_cluster(fs, cluster);
    }

    uint8_t *out = (uint8_t *)buffer;

    /* Read cluster by cluster */
    while (bytes_read < size && cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(fs, cluster);

        uint8_t first_s = 0, last_s = 0, found = 0;
        for (uint8_t s = 0; s < fs->sectors_per_cluster; s++) {
            uint32_t sec_start = pos + (uint32_t)s * 512;
            if (sec_start + 512 <= offset) continue;
            if (sec_start >= offset + size) break;
            if (!found) { first_s = s; found = 1; }
            last_s = s;
        }
        if (!found) break;   /* every cluster the outer loop visits overlaps the window */

        uint8_t run = last_s - first_s + 1;
        if (blk_read(fs->drive_type, fs->drive, lba + first_s, run, cluster_buf) != 0)
            return bytes_read > 0 ? (int)bytes_read : -1;

        for (uint8_t s = first_s; s <= last_s; s++) {
            uint32_t sec_start = pos + (uint32_t)s * 512;

            /* How much of this 512-byte sector do we need? */
            uint32_t copy_start = (offset > sec_start) ? (offset - sec_start) : 0;
            uint32_t copy_len   = 512 - copy_start;
            if (copy_len > size - bytes_read)
                copy_len = size - bytes_read;

            memcpy(&out[bytes_read],
                   &cluster_buf[(uint32_t)(s - first_s) * 512 + copy_start], (int)copy_len);
            bytes_read += copy_len;
        }

        pos += cluster_size;
        cluster = fat32_next_cluster(fs, cluster);
    }

    return (int)bytes_read;
}

/* Patch a node's on-disk directory entry (size + first cluster) to match
 * what's currently in the vfs_node_t. Called after fat32_write changes
 * either — a fresh vfs_node from a later finddir/open must see the new
 * size and, for a just-created file, its now-allocated first cluster. No-op
 * for the volume root, which has no dirent of its own (has_dirent == 0). */
static int fat32_flush_dirent(fat32_file_info_t *fi, vfs_node_t *node) {
    if (!fi->has_dirent) return 0;

    uint8_t buf[512];
    if (blk_read(fi->fs->drive_type, fi->fs->drive, fi->dirent_lba, 1, buf) != 0)
        return -1;

    fat32_dirent_t *de = (fat32_dirent_t *)&buf[fi->dirent_off];
    de->size         = (uint32_t)node->size;
    de->cluster_high = (uint16_t)(node->inode >> 16);
    de->cluster_low  = (uint16_t)(node->inode & 0xFFFF);

    return blk_write(fi->fs->drive_type, fi->fs->drive, fi->dirent_lba, 1, buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_write — write bytes to a file, growing it as needed
 *
 *  Mirrors fat32_read's cluster-walk (including the same per-cluster
 *  contiguous-run batching — one blk_read + one blk_write per cluster
 *  instead of one PIO command pair per 512-byte sector) but read-modify-
 *  writes the run instead of copying out of it: a write can cover only
 *  part of a sector (or part of the run), so the untouched bytes have to
 *  survive the round trip. Batching is scoped to just the overlapping run,
 *  not the whole cluster — writing back sectors nobody asked to change
 *  would be wasted I/O for a small write into a large cluster, the exact
 *  case this optimization is supposed to help, not hurt.
 *
 *  Unlike a plain overwrite, this can extend the file: any write starting
 *  at or before the current EOF (offset <= node->size — a pure "hole" past
 *  EOF isn't supported, nothing in this codebase needs sparse files) that
 *  reaches past node->size allocates however many more clusters are needed
 *  via fat32_alloc_run (contiguous runs, not one cluster at a time — see
 *  its own doc comment), links them onto the chain's tail, and zeroes
 *  each new cluster on disk before the RMW loop below touches it — a freshly
 *  allocated cluster's previous disk contents must never leak out as file
 *  data. node->size and the on-disk dirent (fat32_flush_dirent) are updated
 *  at the end to match how far the write actually reached; a first write to
 *  a freshly fat32_create()'d file (node->inode == 0, no cluster yet) is
 *  handled the same way, treating it as growing from zero.
 *
 *  Returns number of bytes actually written, or negative on error.
 * ══════════════════════════════════════════════════════════════════════════ */

int fat32_write(vfs_node_t *node, uint64_t offset, uint32_t size, const void *buffer) {
    fat32_file_info_t *fi = (fat32_file_info_t *)node->fs_private;
    if (!fi) return -1;
    fat32_fs_t *fs = fi->fs;
    if (!(node->type & VFS_FILE)) return -1;
    if (size == 0) return 0;
    if (offset > node->size) return -1;   /* sparse holes aren't supported */

    uint32_t cluster_size = (uint32_t)fs->sectors_per_cluster * 512;
    uint64_t new_end      = offset + size;

    /* First-ever write to a cluster-less (freshly created, zero-length)
     * file — grab its first cluster now. */
    if (node->inode == 0) {
        uint32_t first = fat32_alloc_cluster(fs);
        if (!first) return -1;
        node->inode = first;
    }

    /* Walk the existing chain to its tail, counting clusters already
     * allocated, then append as many more as this write needs. */
    uint32_t allocated_clusters = 1;
    uint32_t tail               = node->inode;
    for (uint32_t n = fat32_next_cluster(fs, tail); n < FAT32_EOC;
         n = fat32_next_cluster(fs, tail)) {
        tail = n;
        allocated_clusters++;
    }

    uint32_t needed_clusters = (uint32_t)((new_end + cluster_size - 1) / cluster_size);
    if (needed_clusters == 0) needed_clusters = 1;

    while (allocated_clusters < needed_clusters) {
        uint32_t want = needed_clusters - allocated_clusters;
        uint32_t got  = 0;
        uint32_t run_start = fat32_alloc_run(fs, want, &got);
        if (!run_start) break;   /* volume full — write comes up short, handled below */

        /* Zero every cluster in the run before it's linked onto the file
         * — a freshly allocated cluster's previous disk contents must
         * never leak out as file data. The run is contiguous in LBA
         * space (that's the whole point of allocating it as a run), so
         * this is done in cluster_buf-sized chunks rather than one
         * blk_write per cluster — chunked, not a single call, because
         * `got` clusters can add up to more sectors than cluster_buf (one
         * cluster's worth, FAT32_MAX_CLUSTER_SECTORS) holds. */
        uint64_t run_lba       = cluster_to_lba(fs, run_start);
        uint32_t total_sectors = got * (uint32_t)fs->sectors_per_cluster;
        uint32_t zeroed        = 0;
        int      zero_ok       = 1;
        while (zeroed < total_sectors) {
            uint32_t chunk = total_sectors - zeroed;
            if (chunk > FAT32_MAX_CLUSTER_SECTORS) chunk = FAT32_MAX_CLUSTER_SECTORS;
            memset(cluster_buf, 0, (size_t)chunk * 512);
            if (blk_write(fs->drive_type, fs->drive, run_lba + zeroed, chunk, cluster_buf) != 0) {
                zero_ok = 0;
                break;
            }
            zeroed += chunk;
        }
        if (!zero_ok) break;
        if (fat32_set_fat_entry(fs, tail, run_start) != 0)
            break;

        tail = run_start + got - 1;
        allocated_clusters += got;
    }

    /* Clamp the write to whatever got actually allocated (matters only if
     * fat32_alloc_cluster ran out mid-loop above). */
    uint64_t writable_end = (uint64_t)allocated_clusters * cluster_size;
    if (new_end > writable_end) {
        new_end = writable_end;
        size = (new_end > offset) ? (uint32_t)(new_end - offset) : 0;
    }
    if (size == 0) return 0;

    uint32_t cluster       = node->inode;
    uint32_t bytes_written = 0;
    uint64_t pos           = 0;          /* byte offset at cluster start */

    /* Skip whole clusters before the offset */
    while (pos + cluster_size <= offset && cluster < FAT32_EOC) {
        pos += cluster_size;
        cluster = fat32_next_cluster(fs, cluster);
    }

    const uint8_t *in = (const uint8_t *)buffer;

    /* Write cluster by cluster */
    while (bytes_written < size && cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(fs, cluster);

        uint8_t first_s = 0, last_s = 0, found = 0;
        for (uint8_t s = 0; s < fs->sectors_per_cluster; s++) {
            uint32_t sec_start = pos + (uint32_t)s * 512;
            if (sec_start + 512 <= offset) continue;
            if (sec_start >= offset + size) break;
            if (!found) { first_s = s; found = 1; }
            last_s = s;
        }
        if (!found) break;   /* every cluster the outer loop visits overlaps the window */

        uint8_t run = last_s - first_s + 1;

        /* Read the run first so bytes outside each sector's touched range
         * survive the write-back untouched (same reasoning as the old
         * per-sector version, just over `run` sectors in one command). */
        if (blk_read(fs->drive_type, fs->drive, lba + first_s, run, cluster_buf) != 0)
            break;

        for (uint8_t s = first_s; s <= last_s; s++) {
            uint32_t sec_start = pos + (uint32_t)s * 512;

            uint32_t copy_start = (offset > sec_start) ? (offset - sec_start) : 0;
            uint32_t copy_len   = 512 - copy_start;
            if (copy_len > size - bytes_written)
                copy_len = size - bytes_written;

            memcpy(&cluster_buf[(uint32_t)(s - first_s) * 512 + copy_start],
                   &in[bytes_written], (int)copy_len);
            bytes_written += copy_len;
        }

        if (blk_write(fs->drive_type, fs->drive, lba + first_s, run, cluster_buf) != 0)
            break;

        pos += cluster_size;
        cluster = fat32_next_cluster(fs, cluster);
    }

    if (bytes_written == 0) return -1;

    if (offset + bytes_written > node->size)
        node->size = offset + bytes_written;
    fat32_flush_dirent(fi, node);

    return (int)bytes_written;
}

/* Finds (or makes room for, growing the directory by one cluster if
 * nothing fits) a free 32-byte slot in `dir`'s cluster chain and writes a
 * new dirent there — raw 8.3 `name`, and the given cluster/size. Shared by
 * fat32_create (cluster_val=0, size_val=0 — the data cluster is allocated
 * lazily on first write) and fat32_rename's cross-directory move (the
 * file's EXISTING cluster/size, unchanged — nothing about the file's data
 * moves, just which directory names it). A 0x00-marker slot is safe to
 * reuse without moving anything: every entry after it in a well-formed
 * FAT32 directory is 0x00 too (fat32_entry_valid treats 0x00 as "end of
 * directory"), so writing a real entry into that slot still leaves the
 * next slot's 0x00 correctly terminating the listing.
 *
 * Returns 0 on success (out_lba and out_off set to the new entry's on-disk
 * location), or -1 (volume full / I/O error). */
static int fat32_insert_dirent(fat32_fs_t *fs, vfs_node_t *dir, const char *raw,
                               uint32_t cluster_val, uint32_t size_val,
                               uint64_t *out_lba, uint16_t *out_off) {
    uint32_t cluster      = dir->inode;
    uint32_t last_cluster = cluster;
    int      have_slot    = 0;

    while (cluster < FAT32_EOC && !have_slot) {
        uint64_t lba = cluster_to_lba(fs, cluster);

        for (uint8_t s = 0; s < fs->sectors_per_cluster && !have_slot; s++) {
            if (blk_read(fs->drive_type, fs->drive, lba + s, 1, sector_buf) != 0)
                return -1;

            fat32_dirent_t *entries = (fat32_dirent_t *)sector_buf;
            for (uint32_t e = 0; e < 512 / sizeof(fat32_dirent_t); e++) {
                uint8_t first = (uint8_t)entries[e].name[0];
                if (first != 0x00 && first != 0xE5) continue;

                memset(&entries[e], 0, sizeof(fat32_dirent_t));
                memcpy(entries[e].name, raw, 11);
                entries[e].attr         = FAT32_ATTR_ARCHIVE;
                entries[e].cluster_high = (uint16_t)(cluster_val >> 16);
                entries[e].cluster_low  = (uint16_t)(cluster_val & 0xFFFFu);
                entries[e].size         = size_val;

                if (blk_write(fs->drive_type, fs->drive, lba + s, 1, sector_buf) != 0)
                    return -1;

                *out_lba = lba + s;
                *out_off = (uint16_t)(e * sizeof(fat32_dirent_t));
                have_slot = 1;
                break;
            }
        }

        if (!have_slot) {
            last_cluster = cluster;
            cluster = fat32_next_cluster(fs, cluster);
        }
    }

    if (!have_slot) {
        /* Directory is full — grow it by one cluster. */
        uint32_t new_cluster = fat32_alloc_cluster(fs);
        if (!new_cluster) return -1;

        uint64_t new_lba = cluster_to_lba(fs, new_cluster);
        memset(cluster_buf, 0, (size_t)fs->sectors_per_cluster * 512);
        if (blk_write(fs->drive_type, fs->drive, new_lba, fs->sectors_per_cluster, cluster_buf) != 0)
            return -1;
        if (fat32_set_fat_entry(fs, last_cluster, new_cluster) != 0)
            return -1;

        memset(sector_buf, 0, 512);
        fat32_dirent_t *entries = (fat32_dirent_t *)sector_buf;
        memcpy(entries[0].name, raw, 11);
        entries[0].attr         = FAT32_ATTR_ARCHIVE;
        entries[0].cluster_high = (uint16_t)(cluster_val >> 16);
        entries[0].cluster_low  = (uint16_t)(cluster_val & 0xFFFFu);
        entries[0].size         = size_val;
        if (blk_write(fs->drive_type, fs->drive, new_lba, 1, sector_buf) != 0)
            return -1;

        *out_lba = new_lba;
        *out_off = 0;
        have_slot = 1;
    }

    return have_slot ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_create — create a new, empty file inside a directory
 *
 *  The new entry gets cluster 0 and size 0; fat32_write allocates the
 *  actual first data cluster lazily on the first real write, same as a
 *  freshly formatted FAT32 volume's own empty files.
 *
 *  Returns the new vfs_node_t, or NULL if `name` doesn't fit this driver's
 *  8.3-only naming (fat32_parse_83), `dir` isn't a directory, or the volume
 *  is full.
 * ══════════════════════════════════════════════════════════════════════════ */

vfs_node_t *fat32_create(vfs_node_t *dir, const char *name) {
    fat32_file_info_t *dir_fi = (fat32_file_info_t *)dir->fs_private;
    if (!dir_fi) return 0;
    fat32_fs_t *fs = dir_fi->fs;
    if (!(dir->type & VFS_DIRECTORY)) return 0;

    char raw[11];
    if (!fat32_parse_83(name, raw)) return 0;

    uint64_t found_lba;
    uint16_t found_off;
    if (fat32_insert_dirent(fs, dir, raw, 0, 0, &found_lba, &found_off) != 0)
        return 0;

    vfs_node_t *node = alloc_node();
    if (!node) return 0;
    fat32_file_info_t *fi = alloc_finfo();
    if (!fi) return 0;

    fat32_format_83(raw, node->name);
    node->size   = 0;
    node->inode  = 0;   /* fat32_write allocates the first cluster lazily */
    node->type   = VFS_FILE;
    node->read   = fat32_read;
    node->write  = fat32_write;
    node->parent = dir;

    fi->fs         = fs;
    fi->has_dirent = 1;
    fi->dirent_lba = found_lba;
    fi->dirent_off = found_off;
    node->fs_private = fi;

    return node;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_rename — move/rename a file within the same volume
 *
 *  Same directory (old_dir == new_dir): the cheapest case — overwrite the
 *  existing entry's raw name bytes in place, cluster/size untouched.
 *
 *  Different directory: inserts a new entry in new_dir carrying the SAME
 *  cluster/size as the original (fat32_insert_dirent — the same slot-
 *  finding/directory-growth logic fat32_create uses), then clears the old
 *  entry (0xE5) WITHOUT touching the FAT chain — unlike fat32_unlink,
 *  which frees it, because the file keeps existing, just under a new name/
 *  location. If inserting the new entry fails (volume full), the old entry
 *  is left untouched and the file is unaffected — the move either fully
 *  happens or doesn't happen at all, never a half state.
 *
 *  Only regular files — no rmdir semantics, same limit as fat32_unlink.
 * ══════════════════════════════════════════════════════════════════════════ */

int fat32_rename(vfs_node_t *old_dir, const char *old_name,
                 vfs_node_t *new_dir, const char *new_name) {
    fat32_file_info_t *old_dir_fi = (fat32_file_info_t *)old_dir->fs_private;
    if (!old_dir_fi) return -1;
    fat32_fs_t *fs = old_dir_fi->fs;
    if (!(old_dir->type & VFS_DIRECTORY) || !(new_dir->type & VFS_DIRECTORY)) return -1;

    char new_raw[11];
    if (!fat32_parse_83(new_name, new_raw)) return -1;

    vfs_node_t *target = fat32_finddir(old_dir, old_name);
    if (!target) return -2;
    if (!(target->type & VFS_FILE)) return -3;

    fat32_file_info_t *target_fi = (fat32_file_info_t *)target->fs_private;

    if (old_dir == new_dir) {
        uint8_t buf[512];
        if (blk_read(fs->drive_type, fs->drive, target_fi->dirent_lba, 1, buf) != 0)
            return -4;
        fat32_dirent_t *de = (fat32_dirent_t *)&buf[target_fi->dirent_off];
        memcpy(de->name, new_raw, 11);
        if (blk_write(fs->drive_type, fs->drive, target_fi->dirent_lba, 1, buf) != 0)
            return -4;
        return 0;
    }

    uint64_t new_lba;
    uint16_t new_off;
    if (fat32_insert_dirent(fs, new_dir, new_raw, target->inode, (uint32_t)target->size,
                            &new_lba, &new_off) != 0)
        return -5;

    uint8_t buf[512];
    if (blk_read(fs->drive_type, fs->drive, target_fi->dirent_lba, 1, buf) != 0)
        return -6;
    buf[target_fi->dirent_off] = 0xE5;
    if (blk_write(fs->drive_type, fs->drive, target_fi->dirent_lba, 1, buf) != 0)
        return -6;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_unlink — delete a file
 *
 *  Frees the whole cluster chain back to the FAT (fat32_set_fat_entry to 0
 *  — the same "free" value fat32_alloc_cluster scans for), then marks the
 *  directory entry deleted (name[0] = 0xE5, the standard FAT convention —
 *  fat32_entry_valid already treats it as "skip this slot", and
 *  fat32_create already treats it as a reusable free slot).
 *
 *  Only regular files — no rmdir semantics (removing a non-empty directory
 *  would also need to recurse or check emptiness, neither of which exists
 *  here). Reuses fat32_finddir for the lookup: it already does the exact
 *  scan+match this needs and (since fs/fat32.c: fat32_finddir) already
 *  returns a fully-populated fat32_file_info_t with the dirent's on-disk
 *  location, so there's no second directory scan to write by hand.
 * ══════════════════════════════════════════════════════════════════════════ */

int fat32_unlink(vfs_node_t *dir, const char *name) {
    fat32_file_info_t *dir_fi = (fat32_file_info_t *)dir->fs_private;
    if (!dir_fi) return -1;
    fat32_fs_t *fs = dir_fi->fs;
    if (!(dir->type & VFS_DIRECTORY)) return -1;

    vfs_node_t *target = fat32_finddir(dir, name);
    if (!target) return -2;
    if (!(target->type & VFS_FILE)) return -3;

    fat32_file_info_t *target_fi = (fat32_file_info_t *)target->fs_private;

    uint32_t cluster = target->inode;
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t next = fat32_next_cluster(fs, cluster);
        fat32_set_fat_entry(fs, cluster, 0);
        cluster = next;
    }

    uint8_t buf[512];
    if (blk_read(fs->drive_type, fs->drive, target_fi->dirent_lba, 1, buf) != 0)
        return -4;
    buf[target_fi->dirent_off] = 0xE5;
    if (blk_write(fs->drive_type, fs->drive, target_fi->dirent_lba, 1, buf) != 0)
        return -4;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_mount — mount a FAT32 partition
 *
 *  Reads the BPB, validates it, computes layout, creates root vfs_node.
 *  Returns 0 on success.
 * ══════════════════════════════════════════════════════════════════════════ */

int fat32_mount(uint8_t drive_type, uint8_t drive, uint64_t part_lba, mount_point_t *mp) {
    /* Read boot sector */
    uint8_t boot[512];
    if (blk_read(drive_type, drive, part_lba, 1, boot) != 0)
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
    if (bpb->sectors_per_cluster > FAT32_MAX_CLUSTER_SECTORS)
        return -3;              /* fat32_read/write's cluster_buf can't hold this */

    /* ── Allocate internal state ── */
    fat32_fs_t *fs = alloc_fs();
    if (!fs) return -4;

    fs->drive               = drive;
    fs->drive_type          = drive_type;
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

    /* Data clusters available, for fat32_alloc_cluster's search bound */
    uint64_t data_sectors  = fs->total_sectors - (fs->data_start_lba - part_lba);
    fs->total_clusters     = (uint32_t)(data_sectors / fs->sectors_per_cluster);
    fs->next_free_hint     = 2;

    /* Copy & trim volume label */
    for (int i = 0; i < 11; i++)
        fs->volume_label[i] = bpb->volume_label[i];
    fs->volume_label[11] = '\0';
    for (int i = 10; i >= 0 && fs->volume_label[i] == ' '; i--)
        fs->volume_label[i] = '\0';

    /* ── Create root VFS node ── */
    vfs_node_t *root = alloc_node();
    if (!root) return -5;
    fat32_file_info_t *root_fi = alloc_finfo();
    if (!root_fi) return -5;

    root->name[0]    = '/';
    root->name[1]    = '\0';
    root->type       = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root->inode      = fs->root_cluster;
    root->size       = 0;
    root->parent     = 0;
    root->readdir    = fat32_readdir;
    root->finddir    = fat32_finddir;
    root->create     = fat32_create;
    root->unlink     = fat32_unlink;
    root->rename     = fat32_rename;

    root_fi->fs         = fs;
    root_fi->has_dirent = 0;   /* root isn't referenced by any dirent */
    root->fs_private    = root_fi;

    /* ── Fill the mount point ── */
    mp->root         = root;
    mp->drive        = drive;
    mp->fs_type      = FS_FAT32;
    mp->part_lba     = part_lba;
    mp->active       = 1;

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  fat32_defrag / fat32_defrag_one — relocate a fragmented file's data into
 *  a single contiguous run
 *
 *  fat32_alloc_run (see its own doc comment, up near fat32_alloc_cluster)
 *  only fixes fragmentation going forward — it can't do anything about a
 *  file that was already scattered before this session's allocator was
 *  fixed, or one that still fragmented anyway because no big-enough single
 *  run existed at the time it grew. This is what actually moves an
 *  existing file's clusters together.
 *
 *  Ordering is the whole point of doing this safely: the new run is
 *  allocated and completely filled with a copy of the file's data FIRST,
 *  while the directory entry still points at the OLD chain — so the file
 *  stays fully intact and readable through any failure up to that point,
 *  and bailing out just means freeing the unused new run. Only once every
 *  cluster has been copied does the dirent get repointed at the new run
 *  (fat32_flush_dirent, one sector write) — the single moment the file
 *  actually "moves". Only after THAT succeeds is the old chain freed. A
 *  crash between the dirent update and freeing the old chain just leaks
 *  those old clusters as unreferenced-but-still-marked-used space (wasted,
 *  recoverable by a future fsck-style scan) rather than losing or
 *  corrupting anything — never the reverse (free-then-copy), which could
 *  actually destroy data on a crash mid-copy.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Returns 1 if `node` was moved, 0 if it was already one contiguous run
 * (no action taken), -1 if it couldn't be moved (no single free run big
 * enough exists, or an I/O error) — either way the file is left exactly as
 * it was. */
static int fat32_defrag_one(fat32_fs_t *fs, vfs_node_t *node) {
    fat32_file_info_t *fi = (fat32_file_info_t *)node->fs_private;
    if (!fi || !fi->has_dirent) return -1;
    if (node->inode == 0) return 0;   /* empty file, no clusters at all */

    /* Walk the existing chain once: count its length and check whether
     * it's already contiguous (cluster numbers increasing by exactly 1
     * each step) — the overwhelmingly common case once fat32_alloc_run has
     * been running for a while, not worth touching at all. */
    uint32_t old_start   = node->inode;
    uint32_t chain_len   = 1;
    uint32_t prev        = old_start;
    int      already_ok  = 1;
    for (uint32_t c = fat32_next_cluster(fs, prev); c < FAT32_EOC;
         c = fat32_next_cluster(fs, prev)) {
        if (c != prev + 1) already_ok = 0;
        prev = c;
        chain_len++;
    }
    if (already_ok) return 0;

    uint32_t got = 0;
    uint32_t new_start = fat32_alloc_run(fs, chain_len, &got);
    if (!new_start || got != chain_len) {
        if (new_start) {
            for (uint32_t i = 0; i < got; i++)
                fat32_set_fat_entry(fs, new_start + i, 0);
        }
        return -1;   /* no single run big enough right now — leave it fragmented */
    }

    /* Copy the file's data, old chain -> new run, in logical order. Plain
     * cluster-by-cluster (not batched the way fat32_read/write batch
     * contiguous runs) — the OLD side isn't contiguous by definition here,
     * so there's nothing to batch on that end anyway. */
    uint32_t old_c = old_start;
    for (uint32_t i = 0; i < chain_len; i++) {
        uint64_t old_lba = cluster_to_lba(fs, old_c);
        uint64_t new_lba = cluster_to_lba(fs, new_start + i);

        if (blk_read(fs->drive_type, fs->drive, old_lba, fs->sectors_per_cluster, cluster_buf) != 0 ||
            blk_write(fs->drive_type, fs->drive, new_lba, fs->sectors_per_cluster, cluster_buf) != 0) {
            for (uint32_t j = 0; j < chain_len; j++)
                fat32_set_fat_entry(fs, new_start + j, 0);
            return -1;   /* original chain untouched, nothing repointed yet */
        }
        old_c = fat32_next_cluster(fs, old_c);
    }

    /* Every byte is safely at the new location — repoint the dirent. */
    node->inode = new_start;
    if (fat32_flush_dirent(fi, node) != 0) {
        node->inode = old_start;   /* restore in-memory state to match disk */
        for (uint32_t j = 0; j < chain_len; j++)
            fat32_set_fat_entry(fs, new_start + j, 0);
        return -1;
    }

    /* Dirent now points at the new run — free the old chain. */
    old_c = old_start;
    for (uint32_t i = 0; i < chain_len; i++) {
        uint32_t next = fat32_next_cluster(fs, old_c);
        fat32_set_fat_entry(fs, old_c, 0);
        old_c = next;
    }

    return 1;
}

int fat32_defrag(vfs_node_t *dir_node, uint32_t *out_scanned,
                 uint32_t *out_moved, uint32_t *out_skipped) {
    fat32_file_info_t *dir_fi = (fat32_file_info_t *)dir_node->fs_private;
    if (!dir_fi) return -1;
    fat32_fs_t *fs = dir_fi->fs;

    uint32_t scanned = 0, moved = 0, skipped = 0;

    for (uint32_t idx = 0; ; idx++) {
        dirent_t *de = fat32_readdir(dir_node, idx);
        if (!de) break;
        if (de->type & VFS_DIRECTORY) continue;

        /* fat32_readdir hands back a pointer to a single shared static —
         * copy the name out now, before the next loop iteration's own
         * readdir call overwrites it (fat32_finddir below doesn't touch
         * it, only readdir does). */
        char name[MAX_FILENAME];
        int i = 0;
        while (de->name[i] && i < MAX_FILENAME - 1) { name[i] = de->name[i]; i++; }
        name[i] = '\0';

        vfs_node_t *node = fat32_finddir(dir_node, name);
        if (!node) continue;

        scanned++;
        int rc = fat32_defrag_one(fs, node);
        if (rc == 1) moved++;
        else if (rc < 0) skipped++;
    }

    if (out_scanned) *out_scanned = scanned;
    if (out_moved)   *out_moved   = moved;
    if (out_skipped) *out_skipped = skipped;
    return 0;
}
