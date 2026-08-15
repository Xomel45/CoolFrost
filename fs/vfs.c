#include "vfs.h"
#include <stddef.h>
#include "fat32.h"
#include "ext2.h"
#include "ntfs.h"
#include "xfs.h"
#include "../drivers/ata.h"
#include "../drivers/nvme.h"
#include "../drivers/ahci.h"
#include "../libc/mem.h"
#include "../cpu/sched.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Global tables
 * ══════════════════════════════════════════════════════════════════════════ */

static mount_point_t     mount_table[MAX_MOUNTS];

/* fd_table is per-task (task_t.fd_table, cpu/sched.h), not global — this
 * helper just resolves "the caller's table" so vfs_open/close/read/write/
 * seek keep their old signatures (no task_t* parameter to plumb through
 * every call site in kernel.c/elf.c/syscall.c). Safe any time after
 * sched_init(): cur_task is &idle_task from boot, before sched_run() is
 * even called, and idle_task IS the context the early auto-mount directory
 * listing and every shell command run in. */
static inline file_descriptor_t *fdt(void) {
    return sched_current_task()->fd_table;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Init
 * ══════════════════════════════════════════════════════════════════════════ */

void vfs_init(void) {
    memset(mount_table, 0, sizeof(mount_table));
    /* fd tables live in task_t now — each one is reset when its task_pool
     * slot is (re)used, see cpu/sched.c: sched_submit_prio/_user/_user_as.
     * idle_task's is BSS-zeroed at boot. Nothing to do here. */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Simple prefix match: does `str` start with `prefix`? */
static int prefix_match(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

/* strlen without including string.h (which has non-const signatures) */
static int vfs_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Copy src into dst, max `max` chars (always null-terminate) */
static void vfs_strcpy(char *dst, const char *src, int max) {
    int i;
    for (i = 0; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Find the mount point whose path is the longest prefix of `path`.
 * Returns NULL if none match.  Sets *remainder to the part after the prefix. */
static mount_point_t *find_mount(const char *path, const char **remainder) {
    mount_point_t *best = 0;
    int best_len = 0;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) continue;
        int mlen = vfs_strlen(mount_table[i].path);
        if (prefix_match(path, mount_table[i].path) && mlen > best_len) {
            /* Make sure the match ends at a '/' boundary or exact match */
            char after = path[mlen];
            if (after == '\0' || after == '/') {
                best_len = mlen;
                best = &mount_table[i];
            }
        }
    }

    if (best && remainder) {
        const char *r = path + best_len;
        if (*r == '/') r++;
        *remainder = r;
    }
    return best;
}

/* Walk a slash-separated path from a starting node.
 * Returns the final vfs_node_t, or NULL if any component is not found. */
static vfs_node_t *walk_path(vfs_node_t *root, const char *relpath) {
    if (!relpath || !*relpath) return root;

    vfs_node_t *node = root;
    char component[MAX_FILENAME];

    while (*relpath) {
        /* Skip leading slashes */
        while (*relpath == '/') relpath++;
        if (!*relpath) break;

        /* Extract one path component */
        int j = 0;
        while (*relpath && *relpath != '/' && j < MAX_FILENAME - 1)
            component[j++] = *relpath++;
        component[j] = '\0';

        if (!node->finddir) return 0;
        node = node->finddir(node, component);
        if (!node) return 0;
    }

    return node;
}

/* Return the first active mount (used for relative paths) */
static mount_point_t *first_mount(void) {
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mount_table[i].active) return &mount_table[i];
    return 0;
}

/* Resolve an absolute or relative path straight to its vfs_node_t — the
 * "find the mount, walk the rest" pairing vfs_open does inline (it also
 * needs the mount_point_t/remainder themselves for its O_CREATE fallback,
 * so it isn't rewritten to call this), used by anything that just wants
 * the node and nothing else. Returns NULL if no mount matches or any path
 * component isn't found. */
static vfs_node_t *resolve_path(const char *path) {
    mount_point_t *mp;
    const char *relpath;

    if (path[0] == '/') {
        mp = find_mount(path, &relpath);
        if (!mp) return 0;
    } else {
        mp = first_mount();
        if (!mp) return 0;
        relpath = path;
    }

    return walk_path(mp->root, relpath);
}

/* Splits `relpath` (already relative to `mp`'s root) into "the directory
 * node containing its final component" and "that component's name" (copied
 * into `name_out`, a caller-owned buffer at least MAX_FILENAME long) — same
 * manual component-splitting style walk_path already uses, no generic
 * strtok in this codebase (see project memory on the `exec` argv
 * tokenizer). Shared by vfs_open's O_CREATE fallback and vfs_unlink —
 * anything that needs "the parent of X" rather than X itself. Returns NULL
 * if the parent directory itself doesn't exist. */
static vfs_node_t *split_parent(mount_point_t *mp, const char *relpath,
                                char *name_out, int name_out_max) {
    const char *last_slash = 0;
    for (const char *p = relpath; *p; p++)
        if (*p == '/') last_slash = p;

    if (!last_slash) {
        vfs_strcpy(name_out, relpath, name_out_max);
        return mp->root;
    }

    char dirpath[MAX_PATH];
    int dlen = (int)(last_slash - relpath);
    if (dlen >= MAX_PATH) return 0;
    for (int i = 0; i < dlen; i++) dirpath[i] = relpath[i];
    dirpath[dlen] = '\0';

    vfs_strcpy(name_out, last_slash + 1, name_out_max);
    return walk_path(mp->root, dirpath);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_mount — mount a partition at a given path
 *
 *  Reads the MBR, finds the partition, detects filesystem, calls the
 *  appropriate FS-specific mount function (currently only FAT32).
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_mount(uint8_t drive, uint8_t partition, const char *mount_path) {
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;   /* no free mount slots */

    /* Read partition table */
    mbr_partition_t parts[4];
    if (ata_read_partitions(drive, parts) != 0)
        return -2;              /* disk read / bad MBR */

    if (parts[partition].type == 0x00)
        return -3;              /* empty partition     */

    /* Store the path */
    vfs_strcpy(mount_table[slot].path, mount_path, 64);
    mount_table[slot].partition    = partition;
    mount_table[slot].part_sectors = (uint64_t)parts[partition].sector_count;

    /* Detect filesystem by partition type byte */
    uint8_t ptype = parts[partition].type;

    if (ptype == 0x0B || ptype == 0x0C) {
        /* FAT32 or FAT32-LBA */
        return fat32_mount(DRIVE_TYPE_ATA, drive, parts[partition].lba_start,
                           &mount_table[slot]);
    }

    if (ptype == 0x83) {
        /* "Linux filesystem" — real Linux doesn't reserve a separate MBR
         * type byte for xfs vs ext2/3/4 either, it probes the superblock
         * at mount time. Same here: try ext2 first, and only on a
         * genuine bad-magic mismatch (-2, not any other failure) try xfs
         * — an xfs volume's ext2-magic check at byte 1080 will reliably
         * miss (xfs's own magic lives at byte 0), so this never
         * mistakes one for the other. */
        int rc = ext2_mount(DRIVE_TYPE_ATA, drive, parts[partition].lba_start,
                            &mount_table[slot]);
        if (rc == -2)
            return xfs_mount(DRIVE_TYPE_ATA, drive, parts[partition].lba_start,
                             &mount_table[slot]);
        return rc;
    }

    if (ptype == 0x07) {
        /* NTFS / HPFS — try NTFS */
        return ntfs_mount(DRIVE_TYPE_ATA, drive, parts[partition].lba_start,
                          &mount_table[slot]);
    }

    /* Unsupported FS */
    mount_table[slot].active = 0;
    return -4;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_mount_gpt — mount a GPT partition by its LBA range
 *
 *  GPT doesn't use MBR type bytes, so we probe the partition directly
 *  by reading its first sector and checking for FAT32 BPB markers.
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_mount_gpt(uint8_t drive, uint64_t lba_start, uint64_t sector_count,
                  const char *mount_path) {
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    vfs_strcpy(mount_table[slot].path, mount_path, 64);
    mount_table[slot].partition    = 0;
    mount_table[slot].part_sectors = sector_count;

    /* Try FAT32: read the boot sector and check BPB markers */
    uint8_t boot[512];
    if (blk_read(DRIVE_TYPE_ATA, drive, lba_start, 1, boot) != 0) {
        return -2;
    }

    /* Quick FAT32 detection: bytes_per_sector==512, root_entry_count==0,
     * fat_size_16==0, and FS type string "FAT32   " at offset 82 */
    uint16_t bps   = *(uint16_t *)&boot[11];
    uint16_t rec   = *(uint16_t *)&boot[17];
    uint16_t fs16  = *(uint16_t *)&boot[22];

    if (bps == 512 && rec == 0 && fs16 == 0) {
        return fat32_mount(DRIVE_TYPE_ATA, drive, lba_start, &mount_table[slot]);
    }

    /* Try NTFS: OEM ID "NTFS" at offset 3 */
    if (boot[3] == 'N' && boot[4] == 'T' &&
        boot[5] == 'F' && boot[6] == 'S') {
        return ntfs_mount(DRIVE_TYPE_ATA, drive, lba_start, &mount_table[slot]);
    }

    /* Try ext2/3/4: superblock at byte 1024 (LBA+2), magic at offset 56 */
    uint8_t sb_check[512];
    if (blk_read(DRIVE_TYPE_ATA, drive, lba_start + 2, 1, sb_check) == 0) {
        uint16_t ext_magic = *(uint16_t *)&sb_check[56];
        if (ext_magic == 0xEF53) {
            return ext2_mount(DRIVE_TYPE_ATA, drive, lba_start, &mount_table[slot]);
        }
    }

    /* Unsupported FS */
    mount_table[slot].active = 0;
    return -4;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_mount_nvme_gpt — mount a GPT partition from an NVMe drive
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_mount_nvme_gpt(uint8_t nvme_idx, uint64_t lba_start, uint64_t sector_count,
                       const char *mount_path) {
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    vfs_strcpy(mount_table[slot].path, mount_path, 64);
    mount_table[slot].partition    = 0;
    mount_table[slot].part_sectors = sector_count;

    uint8_t boot[512];
    if (blk_read(DRIVE_TYPE_NVME, nvme_idx, lba_start, 1, boot) != 0)
        return -2;

    uint16_t bps  = *(uint16_t *)&boot[11];
    uint16_t rec  = *(uint16_t *)&boot[17];
    uint16_t fs16 = *(uint16_t *)&boot[22];

    if (bps == 512 && rec == 0 && fs16 == 0)
        return fat32_mount(DRIVE_TYPE_NVME, nvme_idx, lba_start, &mount_table[slot]);

    if (boot[3] == 'N' && boot[4] == 'T' &&
        boot[5] == 'F' && boot[6] == 'S')
        return ntfs_mount(DRIVE_TYPE_NVME, nvme_idx, lba_start, &mount_table[slot]);

    uint8_t sb_check[512];
    if (blk_read(DRIVE_TYPE_NVME, nvme_idx, lba_start + 2, 1, sb_check) == 0) {
        uint16_t ext_magic = *(uint16_t *)&sb_check[56];
        if (ext_magic == 0xEF53)
            return ext2_mount(DRIVE_TYPE_NVME, nvme_idx, lba_start, &mount_table[slot]);
    }

    mount_table[slot].active = 0;
    return -4;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_mount_ahci_gpt — mount a GPT partition from an AHCI SATA drive
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_mount_ahci_gpt(uint8_t ahci_idx, uint64_t lba_start, uint64_t sector_count,
                       const char *mount_path) {
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    vfs_strcpy(mount_table[slot].path, mount_path, 64);
    mount_table[slot].partition    = 0;
    mount_table[slot].part_sectors = sector_count;

    uint8_t boot[512];
    if (blk_read(DRIVE_TYPE_AHCI, ahci_idx, lba_start, 1, boot) != 0)
        return -2;

    uint16_t bps  = *(uint16_t *)&boot[11];
    uint16_t rec  = *(uint16_t *)&boot[17];
    uint16_t fs16 = *(uint16_t *)&boot[22];

    if (bps == 512 && rec == 0 && fs16 == 0)
        return fat32_mount(DRIVE_TYPE_AHCI, ahci_idx, lba_start, &mount_table[slot]);

    if (boot[3] == 'N' && boot[4] == 'T' &&
        boot[5] == 'F' && boot[6] == 'S')
        return ntfs_mount(DRIVE_TYPE_AHCI, ahci_idx, lba_start, &mount_table[slot]);

    uint8_t sb_check[512];
    if (blk_read(DRIVE_TYPE_AHCI, ahci_idx, lba_start + 2, 1, sb_check) == 0) {
        uint16_t ext_magic = *(uint16_t *)&sb_check[56];
        if (ext_magic == 0xEF53)
            return ext2_mount(DRIVE_TYPE_AHCI, ahci_idx, lba_start, &mount_table[slot]);
    }

    mount_table[slot].active = 0;
    return -4;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_mount_vblk_gpt — mount a GPT partition from a VirtIO-blk drive
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_mount_vblk_gpt(uint8_t vblk_idx, uint64_t lba_start, uint64_t sector_count,
                       const char *mount_path) {
    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    vfs_strcpy(mount_table[slot].path, mount_path, 64);
    mount_table[slot].partition    = 0;
    mount_table[slot].part_sectors = sector_count;

    uint8_t boot[512];
    if (blk_read(DRIVE_TYPE_VBLK, vblk_idx, lba_start, 1, boot) != 0)
        return -2;

    uint16_t bps  = *(uint16_t *)&boot[11];
    uint16_t rec  = *(uint16_t *)&boot[17];
    uint16_t fs16 = *(uint16_t *)&boot[22];

    if (bps == 512 && rec == 0 && fs16 == 0)
        return fat32_mount(DRIVE_TYPE_VBLK, vblk_idx, lba_start, &mount_table[slot]);

    if (boot[3] == 'N' && boot[4] == 'T' &&
        boot[5] == 'F' && boot[6] == 'S')
        return ntfs_mount(DRIVE_TYPE_VBLK, vblk_idx, lba_start, &mount_table[slot]);

    uint8_t sb_check[512];
    if (blk_read(DRIVE_TYPE_VBLK, vblk_idx, lba_start + 2, 1, sb_check) == 0) {
        uint16_t ext_magic = *(uint16_t *)&sb_check[56];
        if (ext_magic == 0xEF53)
            return ext2_mount(DRIVE_TYPE_VBLK, vblk_idx, lba_start, &mount_table[slot]);
    }

    mount_table[slot].active = 0;
    return -4;
}

/* ══════════════════════════════════════════════════════════════════════════ */

/* Closes every fd in `table` that references a node under `root` — walks
 * each open node's parent chain up to the mount root. Called once per task
 * (every task has its own table now), not once globally. */
static void close_fds_under(file_descriptor_t *table, vfs_node_t *root) {
    for (int f = 0; f < MAX_FD; f++) {
        if (!table[f].active) continue;
        vfs_node_t *n = table[f].node;
        while (n && n != root) n = n->parent;
        if (n == root) table[f].active = 0;
    }
}

int vfs_umount(const char *mount_path) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) continue;

        int match = 1;
        for (int k = 0; mount_path[k] || mount_table[i].path[k]; k++) {
            if (mount_path[k] != mount_table[i].path[k]) { match = 0; break; }
        }
        if (!match) continue;

        /* Close all fds that reference this mount's nodes, across every
         * task's own table — not just the caller's (sched_current_task()
         * is whichever task is running the `umount` shell command, which
         * says nothing about who else has files open on this mount). */
        close_fds_under(sched_idle_task()->fd_table, mount_table[i].root);
        for (int t = 0; t < sched_task_count(); t++)
            close_fds_under(sched_get_task(t)->fd_table, mount_table[i].root);

        mount_table[i].active = 0;
        mount_table[i].root   = 0;
        return 0;
    }
    return -1;  /* mount path not found */
}

int vfs_defrag(const char *mount_path, uint32_t *out_scanned,
              uint32_t *out_moved, uint32_t *out_skipped) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) continue;

        int match = 1;
        for (int k = 0; mount_path[k] || mount_table[i].path[k]; k++) {
            if (mount_path[k] != mount_table[i].path[k]) { match = 0; break; }
        }
        if (!match) continue;

        if (mount_table[i].fs_type == FS_FAT32)
            return fat32_defrag(mount_table[i].root, out_scanned, out_moved, out_skipped);
        if (mount_table[i].fs_type == FS_EXT2)
            return ext2_defrag(mount_table[i].root, out_scanned, out_moved, out_skipped);

        return -2;   /* not supported for this filesystem type yet */
    }
    return -1;  /* mount path not found */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_open — resolve path, allocate fd
 *
 *  Absolute path:   "/hda1/subdir/file.txt"
 *  Relative path:   "file.txt" (uses first active mount)
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_open(const char *path, uint8_t flags) {
    mount_point_t *mp = 0;
    const char *relpath = 0;

    if (path[0] == '/') {
        mp = find_mount(path, &relpath);
        if (!mp) return -1;
    } else {
        mp = first_mount();
        if (!mp) return -1;
        relpath = path;
    }

    vfs_node_t *node = walk_path(mp->root, relpath);

    if (!node) {
        if (!(flags & O_CREATE)) return -2;   /* file not found */

        char name[MAX_FILENAME];
        vfs_node_t *parent = split_parent(mp, relpath, name, MAX_FILENAME);

        if (!parent || !parent->create || !*name) return -2;
        node = parent->create(parent, name);
        if (!node) return -2;   /* driver couldn't create it (full, bad name, ...) */
    }

    /* Find free fd slot (in the calling task's own table) */
    file_descriptor_t *fd_table = fdt();
    for (int i = 0; i < MAX_FD; i++) {
        if (!fd_table[i].active) {
            fd_table[i].node   = node;
            fd_table[i].offset = (flags & O_APPEND) ? node->size : 0;
            fd_table[i].flags  = flags;
            fd_table[i].active = 1;
            return i;
        }
    }

    return -3;   /* fd table full */
}

/* ══════════════════════════════════════════════════════════════════════════ */

int vfs_close(int fd) {
    file_descriptor_t *fd_table = fdt();
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;
    fd_table[fd].active = 0;
    fd_table[fd].node   = 0;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_read — read from an open file, advance offset
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_read(int fd, void *buffer, size_t size) {
    file_descriptor_t *fd_table = fdt();
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;

    vfs_node_t *node = fd_table[fd].node;
    if (!node || !node->read)    return -1;

    int n = node->read(node, fd_table[fd].offset, (uint32_t)size, buffer);
    if (n > 0)
        fd_table[fd].offset += (uint64_t)n;
    return n;
}

int vfs_seek(int fd, uint64_t offset) {
    file_descriptor_t *fd_table = fdt();
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;

    fd_table[fd].offset = offset;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════ */

int vfs_write(int fd, const void *buffer, size_t size) {
    file_descriptor_t *fd_table = fdt();
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;

    vfs_node_t *node = fd_table[fd].node;
    if (!node || !node->write)   return -1;

    int n = node->write(node, fd_table[fd].offset, (uint32_t)size, buffer);
    if (n > 0)
        fd_table[fd].offset += (uint64_t)n;
    return n;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_readdir — read Nth directory entry from an open directory fd
 * ══════════════════════════════════════════════════════════════════════════ */

dirent_t *vfs_readdir(int fd, uint32_t index) {
    file_descriptor_t *fd_table = fdt();
    if (fd < 0 || fd >= MAX_FD)  return 0;
    if (!fd_table[fd].active)    return 0;

    vfs_node_t *node = fd_table[fd].node;
    if (!node || !node->readdir) return 0;

    return node->readdir(node, index);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_stat / vfs_fstat — file metadata without opening for I/O
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_stat(const char *path, vfs_stat_t *out) {
    vfs_node_t *node = resolve_path(path);
    if (!node) return -1;

    out->size  = node->size;
    out->inode = node->inode;
    out->type  = node->type;
    return 0;
}

int vfs_fstat(int fd, vfs_stat_t *out) {
    file_descriptor_t *fd_table = fdt();
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;

    vfs_node_t *node = fd_table[fd].node;
    if (!node) return -1;

    out->size  = node->size;
    out->inode = node->inode;
    out->type  = node->type;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_unlink — remove a file
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_unlink(const char *path) {
    mount_point_t *mp = 0;
    const char *relpath = 0;

    if (path[0] == '/') {
        mp = find_mount(path, &relpath);
        if (!mp) return -1;
    } else {
        mp = first_mount();
        if (!mp) return -1;
        relpath = path;
    }

    char name[MAX_FILENAME];
    vfs_node_t *parent = split_parent(mp, relpath, name, MAX_FILENAME);
    if (!parent || !parent->unlink || !*name) return -2;

    return parent->unlink(parent, name);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_rename — move/rename a file, same filesystem only
 *
 *  Cross-filesystem renames (old_path and new_path resolving to different
 *  mount_point_t's) are refused outright — moving data between two
 *  filesystem drivers would need an actual byte copy (open old for
 *  reading, create new, copy, unlink old — exactly what a userland
 *  ucopy()+SYS_UNLINK does, see userland/ufileutil.h), not the cheap
 *  detach-and-reattach-the-same-inode move a same-filesystem rename is.
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_rename(const char *old_path, const char *new_path) {
    mount_point_t *old_mp = 0;
    const char *old_rel = 0;
    if (old_path[0] == '/') {
        old_mp = find_mount(old_path, &old_rel);
        if (!old_mp) return -1;
    } else {
        old_mp = first_mount();
        if (!old_mp) return -1;
        old_rel = old_path;
    }

    mount_point_t *new_mp = 0;
    const char *new_rel = 0;
    if (new_path[0] == '/') {
        new_mp = find_mount(new_path, &new_rel);
        if (!new_mp) return -1;
    } else {
        new_mp = first_mount();
        if (!new_mp) return -1;
        new_rel = new_path;
    }

    if (old_mp != new_mp) return -1;   /* different filesystem — refuse, see doc comment above */

    char old_name[MAX_FILENAME];
    char new_name[MAX_FILENAME];
    vfs_node_t *old_dir = split_parent(old_mp, old_rel, old_name, MAX_FILENAME);
    vfs_node_t *new_dir = split_parent(new_mp, new_rel, new_name, MAX_FILENAME);

    if (!old_dir || !new_dir || !old_dir->rename || !*old_name || !*new_name) return -2;

    return old_dir->rename(old_dir, old_name, new_dir, new_name);
}

/* ══════════════════════════════════════════════════════════════════════════ */

mount_point_t *vfs_get_mounts(void) {
    return mount_table;
}
