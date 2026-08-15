#include "usyscall.h"

/* Exercises fs/xfs.c end to end against a real mkfs.xfs-built volume
 * (xfs_part.img, seeded via mkfs.xfs's own -p proto-directory flag at
 * build time — this driver is read-only, nothing here can create files on
 * xfs at runtime):
 *   1. Reads small.txt (a "local" format inline file) and checks its exact
 *      content.
 *   2. Reads big.txt (70000 bytes, "extents" format, spans many blocks)
 *      and checks the whole thing byte-for-byte against the i&0xFF
 *      pattern every other big-file test in this tree uses.
 *   3. Lists the root directory (shortform format — 3 entries) and checks
 *      small.txt/big.txt/manyfiles all show up.
 *   4. Opens a file inside manyfiles/ by full path — manyfiles has 60
 *      entries, forcing XFS's single-block "extents" directory format
 *      (shortform tops out far below that), so this exercises the OTHER
 *      directory layout fs/xfs.c supports, plus generic VFS path
 *      resolution descending through an xfs_finddir call on a directory.
 */

static const char pass_pfx[] = "PASS: ";
static const char fail_pfx[] = "FAIL: ";
static const char nl[]       = "\n";

static void report(int ok, const char *label) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)(ok ? pass_pfx : fail_pfx), 6);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

static uint8_t bigbuf[70000];
static char smallbuf[64];

static int ustreq_name(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void _start(void *arg) {
    (void)arg;

    static const char small_path[] = "/xfs1/small.txt";
    static const char expected_small[] = "hello from a real xfs volume";

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)small_path,
                                     ustrlen(small_path), O_RDONLY);
    report(fd >= 0, "open small.txt (local format) succeeded");
    if (fd >= 0) {
        int64_t n = (int64_t)usyscall3(SYS_READ, (uint64_t)fd,
                                        (uint64_t)(unsigned long)smallbuf, sizeof(smallbuf));
        int ok = (n == (int64_t)(sizeof(expected_small) - 1));
        if (ok) {
            for (uint64_t i = 0; i < sizeof(expected_small) - 1; i++)
                if (smallbuf[i] != expected_small[i]) { ok = 0; break; }
        }
        report(ok, "small.txt content matches exactly");
        usyscall1(SYS_CLOSE, (uint64_t)fd);
    }

    static const char big_path[] = "/xfs1/big.txt";
    fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)big_path,
                             ustrlen(big_path), O_RDONLY);
    report(fd >= 0, "open big.txt (extents format) succeeded");
    if (fd >= 0) {
        uint64_t total = 0;
        int ok = 1;
        for (;;) {
            int64_t n = (int64_t)usyscall3(SYS_READ, (uint64_t)fd,
                                            (uint64_t)(unsigned long)(bigbuf + total),
                                            sizeof(bigbuf) - total);
            if (n <= 0) break;
            total += (uint64_t)n;
        }
        report(total == sizeof(bigbuf), "big.txt read the expected number of bytes");
        for (uint64_t i = 0; i < total && i < sizeof(bigbuf); i++) {
            if (bigbuf[i] != (uint8_t)(i & 0xFF)) { ok = 0; break; }
        }
        report(ok, "big.txt content matches the i&0xFF pattern exactly");
        usyscall1(SYS_CLOSE, (uint64_t)fd);
    }

    static const char root_path[] = "/xfs1";
    fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)root_path,
                             ustrlen(root_path), O_RDONLY);
    report(fd >= 0, "open root directory succeeded");
    if (fd >= 0) {
        int saw_small = 0, saw_big = 0, saw_manyfiles = 0, count = 0;
        dirent_t de;
        for (uint32_t idx = 0; ; idx++) {
            if (usyscall3(SYS_READDIR, (uint64_t)fd, (uint64_t)idx,
                          (uint64_t)(unsigned long)&de) != 0)
                break;
            count++;
            if (ustreq_name(de.name, "small.txt")) saw_small = 1;
            if (ustreq_name(de.name, "big.txt")) saw_big = 1;
            if (ustreq_name(de.name, "manyfiles")) saw_manyfiles = 1;
        }
        report(count == 3, "root directory listing has exactly 3 entries");
        report(saw_small && saw_big && saw_manyfiles, "root directory listing has all 3 expected names");
        usyscall1(SYS_CLOSE, (uint64_t)fd);
    }

    static const char nested_path[] = "/xfs1/manyfiles/file037.txt";
    static const char expected_nested[] = "content 37";
    fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)nested_path,
                             ustrlen(nested_path), O_RDONLY);
    report(fd >= 0, "open file inside manyfiles/ (single-block dir format) succeeded");
    if (fd >= 0) {
        int64_t n = (int64_t)usyscall3(SYS_READ, (uint64_t)fd,
                                        (uint64_t)(unsigned long)smallbuf, sizeof(smallbuf));
        int ok = (n == (int64_t)(sizeof(expected_nested) - 1));
        if (ok) {
            for (uint64_t i = 0; i < sizeof(expected_nested) - 1; i++)
                if (smallbuf[i] != expected_nested[i]) { ok = 0; break; }
        }
        report(ok, "nested file content matches exactly");
        usyscall1(SYS_CLOSE, (uint64_t)fd);
    }

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
