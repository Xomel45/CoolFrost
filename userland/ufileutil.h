#ifndef USERLAND_UFILEUTIL_H
#define USERLAND_UFILEUTIL_H

#include "usyscall.h"

/* File-management helpers that DON'T need any new kernel syscall — both
 * "copy a file" and "search a directory tree by name" are just ordinary
 * sequences of the file API that already exists (SYS_OPEN/READ/FWRITE/
 * READDIR/CLOSE). Unlike userland/umalloc.h (which sits on a real new
 * primitive, SYS_SBRK, because the kernel has no notion of "how much
 * memory is this one allocation" and never should), there's nothing here
 * the kernel needs to know about — "copy" and "search" are userland
 * *policy* built from existing mechanism, not new mechanism. Rename/move
 * is the one operation in this family that genuinely couldn't be built
 * this way (see cpu/syscall.h: SYS_RENAME's doc comment) — it needs the
 * filesystem driver's own cooperation to move a directory entry without
 * copying the underlying data.
 */

#define UCOPY_CHUNK 512

/* Copies `src_path` to `dst_path` — read/create/write/close, nothing more.
 * Same filesystem or not, doesn't matter (unlike SYS_RENAME) — this always
 * moves real bytes through a buffer, so it works across mounts too.
 * dst_path is opened with O_CREATE: if it already exists, this overwrites
 * its content from offset 0 rather than refusing or truncating first — a
 * destination file that keeps its OLD tail past the new content's length
 * still has that stale tail. Callers that care should SYS_UNLINK the
 * destination first. Returns 0 on success, -1 on any open/read/write
 * failure (leaves whatever was already written to dst_path in place —
 * no rollback). */
static inline int ucopy(const char *src_path, const char *dst_path) {
    int64_t src_fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)src_path,
                                        ustrlen(src_path), O_RDONLY);
    if (src_fd < 0) return -1;

    int64_t dst_fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)dst_path,
                                        ustrlen(dst_path), O_WRONLY | O_CREATE);
    if (dst_fd < 0) {
        usyscall1(SYS_CLOSE, (uint64_t)src_fd);
        return -1;
    }

    char buf[UCOPY_CHUNK];
    int ok = 1;
    for (;;) {
        int64_t n = (int64_t)usyscall3(SYS_READ, (uint64_t)src_fd,
                                       (uint64_t)(unsigned long)buf, UCOPY_CHUNK);
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;   /* EOF */

        int64_t w = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)dst_fd,
                                       (uint64_t)(unsigned long)buf, (uint64_t)n);
        if (w != n) { ok = 0; break; }
    }

    usyscall1(SYS_CLOSE, (uint64_t)src_fd);
    usyscall1(SYS_CLOSE, (uint64_t)dst_fd);
    return ok ? 0 : -1;
}

#define UFIND_MAX_DEPTH 8
#define UFIND_MAX_PATH  256

static inline int ufind_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Internal recursive walk — see ufind() below for the public entry point. */
static inline int ufind_rec(const char *dir_path, const char *target_name,
                            char *out_path, uint64_t out_max, int depth) {
    if (depth > UFIND_MAX_DEPTH) return 0;

    int64_t dfd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)dir_path,
                                     ustrlen(dir_path), O_RDONLY);
    if (dfd < 0) return -1;

    dirent_t de;
    int result = 0;

    for (uint32_t idx = 0; ; idx++) {
        if (usyscall3(SYS_READDIR, (uint64_t)dfd, (uint64_t)idx,
                      (uint64_t)(unsigned long)&de) != 0)
            break;

        /* "." / ".." show up as ordinary entries in FAT32 SUBdirectories
         * (unlike ext2, which filters them — see fs/ext2.c: ext2_readdir)
         * — skip both, or a self/parent reference turns into infinite
         * recursion. */
        if (de.name[0] == '.' &&
            (de.name[1] == '\0' || (de.name[1] == '.' && de.name[2] == '\0')))
            continue;

        char child_path[UFIND_MAX_PATH];
        uint64_t dl = ustrlen(dir_path);
        uint64_t nl = ustrlen(de.name);
        if (dl + 1 + nl + 1 > UFIND_MAX_PATH) continue;   /* would overflow — skip this entry */

        uint64_t p = 0;
        for (uint64_t i = 0; i < dl; i++) child_path[p++] = dir_path[i];
        if (p == 0 || child_path[p - 1] != '/') child_path[p++] = '/';
        for (uint64_t i = 0; i < nl; i++) child_path[p++] = de.name[i];
        child_path[p] = '\0';

        if (ufind_streq(de.name, target_name)) {
            uint64_t cl = ustrlen(child_path);
            if (cl + 1 <= out_max) {
                for (uint64_t i = 0; i <= cl; i++) out_path[i] = child_path[i];
                result = 1;
                break;
            }
        }

        if (de.type & VFS_DIRECTORY) {
            int r = ufind_rec(child_path, target_name, out_path, out_max, depth + 1);
            if (r != 0) { result = r; break; }
        }
    }

    usyscall1(SYS_CLOSE, (uint64_t)dfd);
    return result;
}

/* Recursively searches `dir_path` for an entry (file or directory) named
 * EXACTLY `target_name` — no case-folding, no wildcards. FAT32 stores
 * short names uppercase (fs/fat32.c: fat32_parse_83), so a search for a
 * lowercase name won't match a FAT32 entry — the same gotcha
 * userland/dtest.c's test had to account for, not something ufind() works
 * around, since it doesn't know which filesystem it's searching.
 *
 * On a match, copies the full path into `out_path` (caller-owned buffer,
 * at least `out_max` bytes) and returns 1. Returns 0 if nothing matched
 * anywhere in the tree, or -1 if `dir_path` itself couldn't even be
 * opened. Depth-limited (UFIND_MAX_DEPTH) rather than fully general —
 * nothing in this codebase creates deeply nested directory trees. */
static inline int ufind(const char *dir_path, const char *target_name,
                        char *out_path, uint64_t out_max) {
    return ufind_rec(dir_path, target_name, out_path, out_max, 0);
}

#endif
