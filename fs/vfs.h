#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include "../drivers/ata.h"

/* ── Limits ────────────────────────────────────────────────────────────── */
#define MAX_FD          64      /* max open file descriptors  */
#define MAX_MOUNTS      8       /* max simultaneous mounts    */
#define MAX_PATH        256     /* max path length            */
#define MAX_FILENAME    128     /* max filename length        */

/* ── File types ────────────────────────────────────────────────────────── */
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_MOUNTPOINT  0x04

/* ── Open flags ────────────────────────────────────────────────────────── */
#define O_RDONLY        0x01
#define O_WRONLY        0x02
#define O_RDWR          0x03
#define O_CREATE        0x04
#define O_APPEND        0x08

/* ── Filesystem type codes ─────────────────────────────────────────────── */
#define FS_NONE         0
#define FS_FAT16        1
#define FS_FAT32        2
#define FS_EXT2         3
#define FS_NTFS         4
#define FS_XFS          5

/* ── Directory entry (returned by readdir) ─────────────────────────────── */
typedef struct {
    char     name[MAX_FILENAME];
    uint64_t size;
    uint8_t  type;          /* VFS_FILE or VFS_DIRECTORY */
} dirent_t;

/* ── stat() result ───────────────────────────────────────────────────────
 *
 * Metadata about a file/directory without opening it for I/O — just a
 * path lookup (vfs_stat) or a lookup on an fd already open (vfs_fstat).
 * Deliberately only what every vfs_node_t already carries regardless of
 * which FS driver backs it (fs/fat32.c, fs/ext2.c, ...) — no owner/perm/
 * timestamp fields, since nothing in this VFS layer tracks those uniformly
 * across drivers yet.
 */
typedef struct {
    uint64_t size;
    uint32_t inode;          /* FS-specific identifier (fat32: cluster, ext2: inode #) */
    uint8_t  type;            /* VFS_FILE or VFS_DIRECTORY */
} vfs_stat_t;

/* ── VFS node (abstract file or directory) ─────────────────────────────── *
 *
 * Each mounted filesystem creates these nodes. The function pointers
 * let the VFS layer call into the concrete filesystem driver (FAT32, etc.)
 * without knowing the implementation details.
 *
 * Example flow:
 *   vfs_open("/hda1/readme.txt")
 *     → find mount_point for "/hda1"
 *     → mount_point->root->finddir(root, "readme.txt")
 *     → returns a vfs_node_t*
 *     → allocate fd, store node in the caller's task fd table
 *   vfs_read(fd, buf, 100)
 *     → fd_table[fd].node->read(node, offset, 100, buf)
 */
struct vfs_node;

typedef int          (*vfs_read_fn)(struct vfs_node *node, uint64_t offset, uint32_t size, void *buffer);
typedef int          (*vfs_write_fn)(struct vfs_node *node, uint64_t offset, uint32_t size, const void *buffer);
typedef int          (*vfs_open_fn)(struct vfs_node *node, uint8_t flags);
typedef int          (*vfs_close_fn)(struct vfs_node *node);
typedef dirent_t    *(*vfs_readdir_fn)(struct vfs_node *node, uint32_t index);
typedef struct vfs_node *(*vfs_finddir_fn)(struct vfs_node *node, const char *name);
/* Creates a new, empty file named `name` inside directory `node`. Returns
 * the new vfs_node_t (same shape finddir would hand back for it), or NULL
 * if the driver can't (name doesn't fit its on-disk naming scheme, volume
 * full, not a directory, etc). Only set on directory nodes. */
typedef struct vfs_node *(*vfs_create_fn)(struct vfs_node *node, const char *name);
/* Removes the file named `name` from directory `node`. Returns 0 on
 * success, negative on error (not found, `name` is a directory — this
 * driver has no rmdir semantics — or the underlying FS driver won't touch
 * it, e.g. an ext4 extent-based file). Only set on directory nodes. */
typedef int (*vfs_unlink_fn)(struct vfs_node *node, const char *name);
/* Moves/renames `old_name` (inside directory `old_dir`) to `new_name`
 * (inside directory `new_dir` — same node as `old_dir` for a plain rename,
 * a different one for a cross-directory move; both are always on the SAME
 * mounted filesystem, vfs_rename() refuses to call this across mounts).
 * No data is copied either way — same underlying file/cluster-chain/inode,
 * just a different directory entry. Returns 0 on success, negative on
 * error. Only set on directory nodes. */
typedef int (*vfs_rename_fn)(struct vfs_node *old_dir, const char *old_name,
                             struct vfs_node *new_dir, const char *new_name);

typedef struct vfs_node {
    char            name[MAX_FILENAME];
    uint8_t         type;           /* VFS_FILE, VFS_DIRECTORY, VFS_MOUNTPOINT */
    uint64_t        size;           /* file size in bytes (0 for dirs) */
    uint32_t        inode;          /* filesystem-specific identifier */

    /* Operations — set by the concrete filesystem driver */
    vfs_read_fn     read;
    vfs_write_fn    write;
    vfs_open_fn     open;
    vfs_close_fn    close;
    vfs_readdir_fn  readdir;
    vfs_finddir_fn  finddir;
    vfs_create_fn   create;
    vfs_unlink_fn   unlink;
    vfs_rename_fn   rename;

    struct vfs_node *parent;        /* parent directory (NULL for root) */
    void           *fs_private;     /* opaque data for the FS driver
                                     * (e.g., FAT32 cluster number, dir offset) */
} vfs_node_t;

/* ── Mount point ───────────────────────────────────────────────────────── *
 *
 * Maps a path like "/hda1" to a filesystem root on a specific
 * drive + partition. Created by vfs_mount(), removed by vfs_umount().
 */
typedef struct {
    char         path[64];          /* mount path, e.g. "/hda1" */
    vfs_node_t  *root;              /* root directory node      */
    uint8_t      active;            /* 1 = mounted              */
    uint8_t      drive;             /* ATA drive index          */
    uint8_t      partition;         /* partition index (0-3)    */
    uint8_t      fs_type;           /* FS_FAT32, etc.           */
    uint64_t     part_lba;          /* partition start LBA      */
    uint64_t     part_sectors;      /* partition size in sectors */
} mount_point_t;

/* ── File descriptor ───────────────────────────────────────────────────── *
 *
 * Opened by vfs_open(), index into the calling task's own fd_table[MAX_FD]
 * (cpu/sched.h: task_t.fd_table) — per-task, not a single global table, so
 * two tasks can each hand out fd 0 for two completely different files.
 * fs/vfs.c resolves "the calling task" via cpu/sched.h: sched_current_task().
 * Tracks the current read/write offset within the file.
 */
typedef struct {
    vfs_node_t  *node;              /* the opened file/dir      */
    uint64_t     offset;            /* current R/W position     */
    uint8_t      flags;             /* O_RDONLY, O_WRONLY, etc.  */
    uint8_t      active;            /* 1 = in use               */
} file_descriptor_t;

/* ── VFS API (to be implemented in fs/vfs.c) ───────────────────────────── */

void      vfs_init(void);

/* File operations */
int       vfs_open(const char *path, uint8_t flags);
int       vfs_close(int fd);
int       vfs_read(int fd, void *buffer, size_t size);
int       vfs_write(int fd, const void *buffer, size_t size);
int       vfs_seek(int fd, uint64_t offset);
int       vfs_unlink(const char *path);
int       vfs_rename(const char *old_path, const char *new_path);

/* Directory operations */
dirent_t *vfs_readdir(int fd, uint32_t index);
int       vfs_finddir(const char *path, dirent_t *out);

/* Metadata — resolve `path` (or an already-open `fd`) without opening it
 * for I/O. Returns 0 on success, negative if not found / fd not open. */
int       vfs_stat(const char *path, vfs_stat_t *out);
int       vfs_fstat(int fd, vfs_stat_t *out);

/* Mount operations */
int       vfs_mount(uint8_t drive, uint8_t partition, const char *mount_path);
int       vfs_mount_gpt(uint8_t drive, uint64_t lba_start, uint64_t sector_count,
                        const char *mount_path);
/* Mount a GPT partition from an NVMe drive */
int       vfs_mount_nvme_gpt(uint8_t nvme_idx, uint64_t lba_start, uint64_t sector_count,
                              const char *mount_path);
/* Mount a GPT partition from an AHCI SATA drive */
int       vfs_mount_ahci_gpt(uint8_t ahci_idx, uint64_t lba_start, uint64_t sector_count,
                              const char *mount_path);
/* Mount a GPT partition from a VirtIO-blk drive */
int       vfs_mount_vblk_gpt(uint8_t vblk_idx, uint64_t lba_start, uint64_t sector_count,
                              const char *mount_path);
int       vfs_umount(const char *mount_path);

/* Defragment the volume mounted at `mount_path` (must match a mount_table
 * entry's path exactly, same lookup as vfs_umount — not a general file
 * path). Supports FAT32 (fs/fat32.c: fat32_defrag) and ext2/3 (fs/ext2.c:
 * ext2_defrag, direct+single-indirect files only); other filesystem types
 * return -2. out_scanned/out_moved/out_skipped (any of which may be NULL)
 * are filled in the same way the underlying per-filesystem function's
 * are. Returns 0 on success, -1 if mount_path isn't an active mount, -2 if
 * that filesystem type doesn't support defrag yet. */
int       vfs_defrag(const char *mount_path, uint32_t *out_scanned,
                     uint32_t *out_moved, uint32_t *out_skipped);

/* Query */
mount_point_t *vfs_get_mounts(void);

#endif
