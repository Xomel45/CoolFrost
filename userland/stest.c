#include "usyscall.h"

/* Exercises SYS_STAT and SYS_FSTAT (fs/vfs.c: vfs_stat/vfs_fstat) — reading
 * a file's size/type without opening it for I/O:
 *   1. SYS_STAT by path on an existing FAT32 file.
 *   2. Opens that same file and SYS_FSTAT's the fd — must agree with (1).
 *   3. SYS_STAT on a directory (/hda1 itself) — type must read back as dir.
 *   4. SYS_STAT on a path that doesn't exist — must fail, not crash.
 *   5. SYS_STAT on /ext1 (the ext2 mount, expected already mounted by the
 *      shell before this runs) — same code path, different FS driver,
 *      proving stat is generic across fs/fat32.c and fs/ext2.c.
 */

static const char fat_file[]   = "/hda1/greeting.txt";
static const char fat_dir[]    = "/hda1";
static const char missing[]    = "/hda1/nope.xyz";
static const char ext_dir[]    = "/ext1";

static const char stat_fail[]  = "stest: SYS_STAT failed\n";
static const char fstat_fail[] = "stest: SYS_FSTAT failed\n";
static const char open_fail[]  = "stest: open failed\n";
static const char size_hdr[]   = "  size=";
static const char type_hdr[]   = " type=";
static const char is_dir[]     = "DIR";
static const char is_file[]    = "FILE";
static const char is_other[]   = "?";
static const char ok_msg[]     = "stest: correctly failed to stat missing file\n";
static const char bad_msg[]    = "stest: BUG — stat succeeded on a nonexistent path\n";
static const char mismatch[]   = "stest: BUG — stat and fstat disagree\n";
static const char nl[]         = "\n";

static void print_u64(uint64_t v) {
    char buf[20];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t;
    }
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)buf, (uint64_t)n);
}

static void print_stat(const char *label, vfs_stat_t *st) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)size_hdr, sizeof(size_hdr) - 1);
    print_u64(st->size);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)type_hdr, sizeof(type_hdr) - 1);
    /* type is a bitmask (fs/vfs.h) — a mount root is VFS_DIRECTORY|
     * VFS_MOUNTPOINT at once, so this has to test bits, not equality. */
    const char *tname = (st->type & VFS_DIRECTORY) ? is_dir
                       : (st->type & VFS_FILE)      ? is_file
                       : is_other;
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)tname, ustrlen(tname));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

void _start(void *arg) {
    (void)arg;

    /* 1. stat by path */
    vfs_stat_t st_path;
    if (usyscall3(SYS_STAT, (uint64_t)(unsigned long)fat_file, ustrlen(fat_file),
                  (uint64_t)(unsigned long)&st_path) != 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)stat_fail, sizeof(stat_fail) - 1);
        usyscall0(SYS_EXIT);
    }
    print_stat("stest: stat(greeting.txt)", &st_path);

    /* 2. fstat on an open fd of the same file — must agree */
    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)fat_file,
                                     ustrlen(fat_file), O_RDONLY);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail, sizeof(open_fail) - 1);
        usyscall0(SYS_EXIT);
    }
    vfs_stat_t st_fd;
    if (usyscall2(SYS_FSTAT, (uint64_t)fd, (uint64_t)(unsigned long)&st_fd) != 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)fstat_fail, sizeof(fstat_fail) - 1);
        usyscall0(SYS_EXIT);
    }
    usyscall1(SYS_CLOSE, (uint64_t)fd);
    print_stat("stest: fstat(greeting.txt)", &st_fd);

    if (st_path.size != st_fd.size || st_path.type != st_fd.type)
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)mismatch, sizeof(mismatch) - 1);

    /* 3. stat a directory */
    vfs_stat_t st_dir;
    if (usyscall3(SYS_STAT, (uint64_t)(unsigned long)fat_dir, ustrlen(fat_dir),
                  (uint64_t)(unsigned long)&st_dir) == 0)
        print_stat("stest: stat(/hda1)", &st_dir);
    else
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)stat_fail, sizeof(stat_fail) - 1);

    /* 4. stat a path that doesn't exist — must fail cleanly */
    vfs_stat_t st_missing;
    if (usyscall3(SYS_STAT, (uint64_t)(unsigned long)missing, ustrlen(missing),
                  (uint64_t)(unsigned long)&st_missing) != 0)
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)ok_msg, sizeof(ok_msg) - 1);
    else
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)bad_msg, sizeof(bad_msg) - 1);

    /* 5. stat across a different FS driver (ext2) — same code path */
    vfs_stat_t st_ext;
    if (usyscall3(SYS_STAT, (uint64_t)(unsigned long)ext_dir, ustrlen(ext_dir),
                  (uint64_t)(unsigned long)&st_ext) == 0)
        print_stat("stest: stat(/ext1)", &st_ext);
    else
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)stat_fail, sizeof(stat_fail) - 1);

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
