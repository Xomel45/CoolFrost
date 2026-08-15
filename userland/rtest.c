#include "usyscall.h"
#include "ufileutil.h"

/* Exercises SYS_RENAME (fs/fat32.c: fat32_rename, fs/ext2.c: ext2_rename)
 * plus the pure-userland ucopy/ufind helpers (userland/ufileutil.h) built
 * on syscalls that already existed:
 *
 *   FAT32 (/hda1) — same-directory rename only. This driver has no mkdir,
 *   and the test image's FAT32 partition has no pre-existing subdirectory,
 *   so there's no second directory to move INTO — cross-directory move is
 *   tested on ext2 instead, where mke2fs already created one for us
 *   (lost+found).
 *     create rtest.txt -> rename to rtest2.txt -> stat confirms old name
 *     gone, new name present -> ucopy to rtest3.txt -> stat + content
 *     check -> ufind locates it by name.
 *
 *   ext2 (/ext1) — the fuller cycle, using lost+found as a real second
 *   directory:
 *     create rtest.txt -> rename (same dir) to rtest2.txt -> MOVE
 *     (cross-directory) into lost+found -> stat confirms it's gone from
 *     the root and present (with correct content) in lost+found -> ucopy
 *     back out to /ext1/rtest3.txt -> ufind locates BOTH the moved file
 *     (proving the recursive walk actually descends into lost+found) and
 *     the copy.
 */

static const char pass_pfx[] = "PASS: ";
static const char fail_pfx[] = "FAIL: ";
static const char nl[]       = "\n";

static void report(int ok, const char *label) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)(ok ? pass_pfx : fail_pfx), 6);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

static int file_exists(const char *path) {
    vfs_stat_t st;
    return usyscall3(SYS_STAT, (uint64_t)(unsigned long)path, ustrlen(path),
                     (uint64_t)(unsigned long)&st) == 0;
}

static int content_is(const char *path, const char *expect) {
    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path,
                                    ustrlen(path), O_RDONLY);
    if (fd < 0) return 0;
    char buf[64];
    int64_t n = (int64_t)usyscall3(SYS_READ, (uint64_t)fd,
                                   (uint64_t)(unsigned long)buf, sizeof(buf));
    usyscall1(SYS_CLOSE, (uint64_t)fd);
    if (n < 0) return 0;
    uint64_t elen = ustrlen(expect);
    if ((uint64_t)n != elen) return 0;
    for (uint64_t i = 0; i < elen; i++)
        if (buf[i] != expect[i]) return 0;
    return 1;
}

static int rename2(const char *old_path, const char *new_path) {
    return usyscall4(SYS_RENAME, (uint64_t)(unsigned long)old_path, ustrlen(old_path),
                     (uint64_t)(unsigned long)new_path, ustrlen(new_path)) == 0;
}

static void make_file(const char *path, const char *content) {
    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path,
                                    ustrlen(path), O_WRONLY | O_CREATE);
    if (fd < 0) return;
    usyscall3(SYS_FWRITE, (uint64_t)fd, (uint64_t)(unsigned long)content, ustrlen(content));
    usyscall1(SYS_CLOSE, (uint64_t)fd);
}

void _start(void *arg) {
    (void)arg;

    /* ── FAT32: same-directory rename + copy + find ── */
    make_file("/hda1/rtest.txt", "hello\n");

    report(rename2("/hda1/rtest.txt", "/hda1/rtest2.txt"), "fat32: rename succeeded");
    report(!file_exists("/hda1/rtest.txt"), "fat32: old name gone after rename");
    report(file_exists("/hda1/rtest2.txt"), "fat32: new name present after rename");
    report(content_is("/hda1/rtest2.txt", "hello\n"), "fat32: content intact after rename");

    report(ucopy("/hda1/rtest2.txt", "/hda1/rtest3.txt") == 0, "fat32: copy succeeded");
    report(content_is("/hda1/rtest3.txt", "hello\n"), "fat32: copy content matches source");

    char found_path[UFIND_MAX_PATH];
    /* FAT32 short names come back uppercase from readdir (fs/fat32.c:
     * fat32_parse_83) — search for the name the way it's actually stored. */
    report(ufind("/hda1", "RTEST3.TXT", found_path, sizeof(found_path)) == 1,
          "fat32: ufind locates the copy");

    usyscall2(SYS_UNLINK, (uint64_t)(unsigned long)"/hda1/rtest2.txt", ustrlen("/hda1/rtest2.txt"));
    usyscall2(SYS_UNLINK, (uint64_t)(unsigned long)"/hda1/rtest3.txt", ustrlen("/hda1/rtest3.txt"));

    /* ── ext2: same-directory rename, cross-directory move, copy, find ── */
    make_file("/ext1/rtest.txt", "hello\n");

    report(rename2("/ext1/rtest.txt", "/ext1/rtest2.txt"), "ext2: rename succeeded");
    report(!file_exists("/ext1/rtest.txt"), "ext2: old name gone after rename");
    report(file_exists("/ext1/rtest2.txt"), "ext2: new name present after rename");

    report(rename2("/ext1/rtest2.txt", "/ext1/lost+found/rtest2.txt"),
          "ext2: cross-directory move succeeded");
    report(!file_exists("/ext1/rtest2.txt"), "ext2: gone from source dir after move");
    report(file_exists("/ext1/lost+found/rtest2.txt"), "ext2: present in dest dir after move");
    report(content_is("/ext1/lost+found/rtest2.txt", "hello\n"), "ext2: content intact after move");

    report(ucopy("/ext1/lost+found/rtest2.txt", "/ext1/rtest3.txt") == 0, "ext2: copy succeeded");
    report(content_is("/ext1/rtest3.txt", "hello\n"), "ext2: copy content matches source");

    report(ufind("/ext1", "rtest3.txt", found_path, sizeof(found_path)) == 1,
          "ext2: ufind locates the copy");
    report(ufind("/ext1", "rtest2.txt", found_path, sizeof(found_path)) == 1,
          "ext2: ufind recurses into lost+found and finds the moved file");

    usyscall2(SYS_UNLINK, (uint64_t)(unsigned long)"/ext1/lost+found/rtest2.txt",
             ustrlen("/ext1/lost+found/rtest2.txt"));
    usyscall2(SYS_UNLINK, (uint64_t)(unsigned long)"/ext1/rtest3.txt", ustrlen("/ext1/rtest3.txt"));

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
