#include "vfs.h"
#include "fat32.h"
#include "../drivers/ata.h"
#include "../libc/mem.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Global tables
 * ══════════════════════════════════════════════════════════════════════════ */

static mount_point_t     mount_table[MAX_MOUNTS];
static file_descriptor_t fd_table[MAX_FD];

/* ══════════════════════════════════════════════════════════════════════════
 *  Init
 * ══════════════════════════════════════════════════════════════════════════ */

void vfs_init(void) {
    memset(mount_table, 0, sizeof(mount_table));
    memset(fd_table,    0, sizeof(fd_table));
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
    mount_table[slot].part_sectors = parts[partition].sector_count;

    /* Detect filesystem by partition type byte */
    uint8_t ptype = parts[partition].type;

    if (ptype == 0x0B || ptype == 0x0C) {
        /* FAT32 or FAT32-LBA */
        return fat32_mount(drive, parts[partition].lba_start,
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

int vfs_mount_gpt(uint8_t drive, uint32_t lba_start, uint32_t sector_count,
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
    if (ata_read_sectors(drive, lba_start, 1, boot) != 0) {
        return -2;
    }

    /* Quick FAT32 detection: bytes_per_sector==512, root_entry_count==0,
     * fat_size_16==0, and FS type string "FAT32   " at offset 82 */
    uint16_t bps   = *(uint16_t *)&boot[11];
    uint16_t rec   = *(uint16_t *)&boot[17];
    uint16_t fs16  = *(uint16_t *)&boot[22];

    if (bps == 512 && rec == 0 && fs16 == 0) {
        /* Likely FAT32 */
        return fat32_mount(drive, lba_start, &mount_table[slot]);
    }

    /* Unsupported FS */
    mount_table[slot].active = 0;
    return -4;
}

/* ══════════════════════════════════════════════════════════════════════════ */

int vfs_umount(const char *mount_path) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].active) continue;

        int match = 1;
        for (int k = 0; mount_path[k] || mount_table[i].path[k]; k++) {
            if (mount_path[k] != mount_table[i].path[k]) { match = 0; break; }
        }
        if (!match) continue;

        /* Close all fds that reference this mount's nodes */
        for (int f = 0; f < MAX_FD; f++) {
            if (!fd_table[f].active) continue;
            /* Walk up parent chain to see if it belongs to this mount */
            vfs_node_t *n = fd_table[f].node;
            while (n && n != mount_table[i].root) n = n->parent;
            if (n == mount_table[i].root)
                fd_table[f].active = 0;
        }

        mount_table[i].active = 0;
        mount_table[i].root   = 0;
        return 0;
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
    vfs_node_t *node = 0;

    if (path[0] == '/') {
        const char *remainder = 0;
        mount_point_t *mp = find_mount(path, &remainder);
        if (!mp) return -1;
        node = walk_path(mp->root, remainder);
    } else {
        mount_point_t *mp = first_mount();
        if (!mp) return -1;
        node = walk_path(mp->root, path);
    }

    if (!node) return -2;   /* file not found */

    /* Find free fd slot */
    for (int i = 0; i < MAX_FD; i++) {
        if (!fd_table[i].active) {
            fd_table[i].node   = node;
            fd_table[i].offset = 0;
            fd_table[i].flags  = flags;
            fd_table[i].active = 1;
            return i;
        }
    }

    return -3;   /* fd table full */
}

/* ══════════════════════════════════════════════════════════════════════════ */

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;
    fd_table[fd].active = 0;
    fd_table[fd].node   = 0;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_read — read from an open file, advance offset
 * ══════════════════════════════════════════════════════════════════════════ */

int vfs_read(int fd, void *buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;

    vfs_node_t *node = fd_table[fd].node;
    if (!node || !node->read)    return -1;

    int n = node->read(node, fd_table[fd].offset, size, buffer);
    if (n > 0)
        fd_table[fd].offset += (uint32_t)n;
    return n;
}

/* ══════════════════════════════════════════════════════════════════════════ */

int vfs_write(int fd, const void *buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_FD)  return -1;
    if (!fd_table[fd].active)    return -1;

    vfs_node_t *node = fd_table[fd].node;
    if (!node || !node->write)   return -1;

    int n = node->write(node, fd_table[fd].offset, size, buffer);
    if (n > 0)
        fd_table[fd].offset += (uint32_t)n;
    return n;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  vfs_readdir — read Nth directory entry from an open directory fd
 * ══════════════════════════════════════════════════════════════════════════ */

dirent_t *vfs_readdir(int fd, uint32_t index) {
    if (fd < 0 || fd >= MAX_FD)  return 0;
    if (!fd_table[fd].active)    return 0;

    vfs_node_t *node = fd_table[fd].node;
    if (!node || !node->readdir) return 0;

    return node->readdir(node, index);
}

/* ══════════════════════════════════════════════════════════════════════════ */

mount_point_t *vfs_get_mounts(void) {
    return mount_table;
}
