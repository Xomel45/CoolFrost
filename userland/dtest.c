#include "usyscall.h"

/* Exercises SYS_READDIR and SYS_UNLINK (fs/vfs.c: vfs_readdir/vfs_unlink)
 * together, on both FAT32 (/hda1) and ext2 (/ext1, expected already
 * mounted by the shell before this runs):
 *   1. Lists the directory, counting entries and confirming the test file
 *      isn't there yet.
 *   2. Creates it (SYS_OPEN + O_CREATE), writes a few bytes, closes.
 *   3. SYS_STAT confirms it exists with the right size.
 *   4. Lists again — count must be +1, and the name must now show up in
 *      the listing (not just "a file with that name is openable").
 *   5. SYS_UNLINK removes it.
 *   6. SYS_STAT on the same path must now fail.
 *   7. Lists a third time — count back to the original, name gone from
 *      the listing too (not just unopenable).
 *
 * Every check reports PASS/FAIL on its own rather than just printing raw
 * numbers to eyeball — readdir output legitimately varies run to run
 * (whatever else is already on that FS), so "does the delta match" is the
 * only thing worth asserting, not any absolute count.
 */

static const char pass_pfx[] = "PASS: ";
static const char fail_pfx[] = "FAIL: ";
static const char nl[]       = "\n";

/* Case-insensitive — FAT32 short names are always stored uppercase
 * (fs/fat32.c: fat32_parse_83), so "dtest.txt" comes back from readdir as
 * "DTEST.TXT" there. ext2 preserves case exactly, so this is a strict
 * superset of what ext2 needs, not a loosened check for it. */
static int ustreq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void report(int ok, const char *label) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)(ok ? pass_pfx : fail_pfx), 6);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

/* Opens `dirpath` as a directory and walks every SYS_READDIR index until
 * it fails. Returns the entry count, or -1 if the directory itself
 * couldn't be opened. If `target` is non-NULL, *found is set to whether
 * an entry with that exact name showed up. */
static int list_dir(const char *dirpath, const char *target, int *found) {
    if (found) *found = 0;

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)dirpath,
                                     ustrlen(dirpath), O_RDONLY);
    if (fd < 0) return -1;

    dirent_t de;
    int count = 0;
    for (uint32_t idx = 0; ; idx++) {
        if (usyscall3(SYS_READDIR, (uint64_t)fd, (uint64_t)idx,
                      (uint64_t)(unsigned long)&de) != 0)
            break;
        count++;
        if (target && found && ustreq(de.name, target))
            *found = 1;
    }
    usyscall1(SYS_CLOSE, (uint64_t)fd);
    return count;
}

static int file_exists(const char *path) {
    vfs_stat_t st;
    return usyscall3(SYS_STAT, (uint64_t)(unsigned long)path, ustrlen(path),
                     (uint64_t)(unsigned long)&st) == 0;
}

/* Runs the full create -> list -> unlink -> list cycle against one
 * filesystem. `dirpath` is the directory to list ("/hda1" or "/ext1"),
 * `filepath` the full path of the test file inside it, `basename` just
 * its name (what should/shouldn't appear in a directory listing). */
static void run_cycle(const char *dirpath, const char *filepath, const char *basename) {
    int found;
    int before = list_dir(dirpath, basename, &found);
    report(before >= 0, "list before create succeeded");
    report(!found, "target absent from listing before create");

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)filepath,
                                     ustrlen(filepath), O_WRONLY | O_CREATE);
    report(fd >= 0, "create succeeded");
    if (fd < 0) return;

    static const char content[] = "hello\n";
    int64_t n = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                    (uint64_t)(unsigned long)content, sizeof(content) - 1);
    usyscall1(SYS_CLOSE, (uint64_t)fd);
    report(n == (int64_t)(sizeof(content) - 1), "write succeeded");

    report(file_exists(filepath), "stat sees the new file");

    int after = list_dir(dirpath, basename, &found);
    report(after == before + 1, "listing count grew by exactly 1");
    report(found, "target present in listing after create");

    int64_t rc = (int64_t)usyscall2(SYS_UNLINK, (uint64_t)(unsigned long)filepath,
                                     ustrlen(filepath));
    report(rc == 0, "unlink succeeded");

    report(!file_exists(filepath), "stat no longer sees the file");

    int final = list_dir(dirpath, basename, &found);
    report(final == before, "listing count back to original");
    report(!found, "target absent from listing after unlink");
}

void _start(void *arg) {
    (void)arg;

    run_cycle("/hda1", "/hda1/dtest.txt", "dtest.txt");
    run_cycle("/ext1", "/ext1/dtest.txt", "dtest.txt");

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
