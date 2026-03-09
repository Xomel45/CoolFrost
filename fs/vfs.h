#ifndef VFS_H
#define VFS_H

#include <stdint.h>
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

/* ── Directory entry (returned by readdir) ─────────────────────────────── */
typedef struct {
    char     name[MAX_FILENAME];
    uint32_t size;
    uint8_t  type;          /* VFS_FILE or VFS_DIRECTORY */
} dirent_t;

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
 *     → allocate fd, store node in fd table
 *   vfs_read(fd, buf, 100)
 *     → fd_table[fd].node->read(node, offset, 100, buf)
 */
struct vfs_node;

typedef int          (*vfs_read_fn)(struct vfs_node *node, uint32_t offset, uint32_t size, void *buffer);
typedef int          (*vfs_write_fn)(struct vfs_node *node, uint32_t offset, uint32_t size, const void *buffer);
typedef int          (*vfs_open_fn)(struct vfs_node *node, uint8_t flags);
typedef int          (*vfs_close_fn)(struct vfs_node *node);
typedef dirent_t    *(*vfs_readdir_fn)(struct vfs_node *node, uint32_t index);
typedef struct vfs_node *(*vfs_finddir_fn)(struct vfs_node *node, const char *name);

typedef struct vfs_node {
    char            name[MAX_FILENAME];
    uint8_t         type;           /* VFS_FILE, VFS_DIRECTORY, VFS_MOUNTPOINT */
    uint32_t        size;           /* file size in bytes (0 for dirs) */
    uint32_t        inode;          /* filesystem-specific identifier */

    /* Operations — set by the concrete filesystem driver */
    vfs_read_fn     read;
    vfs_write_fn    write;
    vfs_open_fn     open;
    vfs_close_fn    close;
    vfs_readdir_fn  readdir;
    vfs_finddir_fn  finddir;

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
    uint32_t     part_lba;          /* partition start LBA      */
    uint32_t     part_sectors;      /* partition size in sectors */
} mount_point_t;

/* ── File descriptor ───────────────────────────────────────────────────── *
 *
 * Opened by vfs_open(), index into a global fd_table[MAX_FD].
 * Tracks the current read/write offset within the file.
 */
typedef struct {
    vfs_node_t  *node;              /* the opened file/dir      */
    uint32_t     offset;            /* current R/W position     */
    uint8_t      flags;             /* O_RDONLY, O_WRONLY, etc.  */
    uint8_t      active;            /* 1 = in use               */
} file_descriptor_t;

/* ── VFS API (to be implemented in fs/vfs.c) ───────────────────────────── */

void      vfs_init(void);

/* File operations */
int       vfs_open(const char *path, uint8_t flags);
int       vfs_close(int fd);
int       vfs_read(int fd, void *buffer, uint32_t size);
int       vfs_write(int fd, const void *buffer, uint32_t size);

/* Directory operations */
dirent_t *vfs_readdir(int fd, uint32_t index);
int       vfs_finddir(const char *path, dirent_t *out);

/* Mount operations */
int       vfs_mount(uint8_t drive, uint8_t partition, const char *mount_path);
int       vfs_mount_gpt(uint8_t drive, uint32_t lba_start, uint32_t sector_count,
                        const char *mount_path);
int       vfs_umount(const char *mount_path);

/* Query */
mount_point_t *vfs_get_mounts(void);

#endif
