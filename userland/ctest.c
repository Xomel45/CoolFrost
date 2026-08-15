#include "usyscall.h"

/* Exercises SYS_OPEN's O_CREATE and SYS_FWRITE's new growth support
 * (fs/fat32.c: fat32_create/fat32_write) end to end:
 *   1. Creates a brand-new file that doesn't exist yet.
 *   2. Writes 700 bytes in one call — this image's hdd.img formats FAT32 at
 *      1 sector (512 bytes) per cluster, so this alone forces both "first
 *      cluster ever" allocation and one mid-write chain extension.
 *   3. Seeks to the end and appends 400 more, forcing a THIRD cluster to
 *      be linked onto an already-nonzero chain (walks the existing chain
 *      to its tail before extending, rather than just reusing state left
 *      over from step 2).
 *   4. Re-opens fresh (a new vfs_node_t, a real directory walk, not any
 *      kind of cache) and reads the whole thing back, proving the size and
 *      content — not just the in-memory vfs_node_t — actually landed on
 *      disk.
 */

static const char path[]         = "/hda1/newfile.txt";
static const char open_fail[]    = "ctest: open/create failed\n";
static const char write_fail[]   = "ctest: write failed\n";
static const char seek_fail[]    = "ctest: seek failed\n";
static const char wrote_hdr[]    = "ctest: wrote ";
static const char readback_hdr[] = "ctest: read back ";
static const char bytes_sfx[]    = " bytes\n";

static char chunk1[700];
static char chunk2[400];

static void fill(char *buf, uint64_t n, char base) {
    for (uint64_t i = 0; i < n; i++)
        buf[i] = (char)(base + (char)(i % 26));
}

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

void _start(void *arg) {
    (void)arg;

    fill(chunk1, sizeof(chunk1), 'A');
    fill(chunk2, sizeof(chunk2), 'a');

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path,
                                     ustrlen(path), O_WRONLY | O_CREATE);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail, sizeof(open_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    int64_t n1 = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                     (uint64_t)(unsigned long)chunk1, sizeof(chunk1));
    if (n1 != (int64_t)sizeof(chunk1)) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)write_fail, sizeof(write_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    if (usyscall2(SYS_SEEK, (uint64_t)fd, (uint64_t)sizeof(chunk1)) != 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)seek_fail, sizeof(seek_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    int64_t n2 = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                     (uint64_t)(unsigned long)chunk2, sizeof(chunk2));
    if (n2 != (int64_t)sizeof(chunk2)) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)write_fail, sizeof(write_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    usyscall1(SYS_CLOSE, (uint64_t)fd);

    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)wrote_hdr, sizeof(wrote_hdr) - 1);
    print_u64((uint64_t)(n1 + n2));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)bytes_sfx, sizeof(bytes_sfx) - 1);

    fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path, ustrlen(path), O_RDONLY);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail, sizeof(open_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    char buf[256];
    uint64_t total = 0;
    for (;;) {
        int64_t r = (int64_t)usyscall3(SYS_READ, (uint64_t)fd,
                                        (uint64_t)(unsigned long)buf, sizeof(buf));
        if (r <= 0) break;
        total += (uint64_t)r;
    }
    usyscall1(SYS_CLOSE, (uint64_t)fd);

    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)readback_hdr, sizeof(readback_hdr) - 1);
    print_u64(total);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)bytes_sfx, sizeof(bytes_sfx) - 1);

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
